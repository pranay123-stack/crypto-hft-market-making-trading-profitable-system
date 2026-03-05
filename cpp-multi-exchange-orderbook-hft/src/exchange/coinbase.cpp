#include "exchange/coinbase.hpp"

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
#include <chrono>
#include <random>

namespace hft {
namespace exchange {

// ============================================================================
// Constructors/Destructor
// ============================================================================

CoinbaseExchange::CoinbaseExchange(const ExchangeConfig& config)
    : ExchangeBase(config) {
    coinbase_config_.api_key = config.api_key;
    coinbase_config_.api_secret = config.api_secret;
    coinbase_config_.passphrase = config.passphrase;
    coinbase_config_.testnet = config.testnet;
    coinbase_config_.order_rate_limit = config.orders_per_second;
    coinbase_config_.request_rate_limit = config.requests_per_minute;

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "coinbase_rest";
    rest_config.request_timeout_ms = config.rest_timeout_ms;
    rest_config.max_retries = config.rest_max_retries;
    rest_config.rate_limit.requests_per_second = coinbase_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = coinbase_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

CoinbaseExchange::CoinbaseExchange(const CoinbaseConfig& coinbase_config)
    : ExchangeBase(ExchangeConfig{
        .name = "Coinbase",
        .api_key = coinbase_config.api_key,
        .api_secret = coinbase_config.api_secret,
        .passphrase = coinbase_config.passphrase,
        .type = ExchangeType::Spot,
        .testnet = coinbase_config.testnet
      }),
      coinbase_config_(coinbase_config) {

    // Initialize REST client
    network::RestClientConfig rest_config;
    rest_config.base_url = getRestUrl();
    rest_config.name = "coinbase_rest";
    rest_config.rate_limit.requests_per_second = coinbase_config_.order_rate_limit;
    rest_config.rate_limit.requests_per_minute = coinbase_config_.request_rate_limit;

    rest_client_ = std::make_shared<network::RestClient>(rest_config);
}

CoinbaseExchange::~CoinbaseExchange() {
    disconnect();
}

// ============================================================================
// URL Builders
// ============================================================================

std::string CoinbaseExchange::getRestUrl() const {
    // Coinbase Advanced Trade API
    // Sandbox: https://api-public.sandbox.exchange.coinbase.com
    // Production: https://api.coinbase.com
    return coinbase_config_.testnet
        ? "https://api-public.sandbox.exchange.coinbase.com"
        : "https://api.coinbase.com";
}

std::string CoinbaseExchange::getWsUrl() const {
    // Advanced Trade WebSocket (Market Data)
    // Sandbox: wss://advanced-trade-ws.sandbox.coinbase.com
    // Production: wss://advanced-trade-ws.coinbase.com
    return coinbase_config_.testnet
        ? "wss://advanced-trade-ws.sandbox.coinbase.com"
        : "wss://advanced-trade-ws.coinbase.com";
}

std::string CoinbaseExchange::getWsUserUrl() const {
    // Advanced Trade WebSocket (User Order Data)
    // Sandbox: wss://advanced-trade-ws-user.sandbox.coinbase.com
    // Production: wss://advanced-trade-ws-user.coinbase.com
    return coinbase_config_.testnet
        ? "wss://advanced-trade-ws-user.sandbox.coinbase.com"
        : "wss://advanced-trade-ws-user.coinbase.com";
}

// ============================================================================
// Connection Management
// ============================================================================

bool CoinbaseExchange::connect() {
    if (ws_connected_.load()) {
        return true;
    }

    onConnectionStatus(ConnectionStatus::Connecting);

    // Initialize WebSocket client
    network::WebSocketConfig ws_config;
    ws_config.url = getWsUrl();
    ws_config.name = "coinbase_ws";
    ws_config.ping_interval_ms = 30000;  // Coinbase requires heartbeat
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

void CoinbaseExchange::disconnect() {
    if (ws_client_) {
        ws_client_->disconnect();
        ws_client_.reset();
    }

    ws_connected_ = false;
    user_subscribed_ = false;
    onConnectionStatus(ConnectionStatus::Disconnected);
}

bool CoinbaseExchange::isConnected() const {
    return ws_connected_.load();
}

ConnectionStatus CoinbaseExchange::connectionStatus() const {
    return connection_status_.load();
}

// ============================================================================
// Signature Generation
// ============================================================================

std::string CoinbaseExchange::generateTimestamp() const {
    return std::to_string(currentTimeMs() / 1000);
}

std::string CoinbaseExchange::sign(const std::string& timestamp, const std::string& method,
                                    const std::string& request_path, const std::string& body) const {
    // Coinbase Advanced Trade API (Legacy Keys) signature:
    // Sign: timestamp + method + requestPath + body
    // Method: HMAC-SHA256, output as lowercase hex digest
    //
    // Note: For Exchange API (CB-ACCESS-PASSPHRASE required), the secret is base64
    // encoded and output is also base64. For Advanced Trade Legacy Keys, the secret
    // is used directly and output is hex.

    std::string pre_hash = timestamp + method + request_path + body;
    std::string secret_key;
    bool use_base64_encoding = !coinbase_config_.passphrase.empty();

    if (use_base64_encoding) {
        // Exchange API: Base64 decode the secret key first
        BIO* bio = BIO_new_mem_buf(coinbase_config_.api_secret.c_str(), -1);
        BIO* b64 = BIO_new(BIO_f_base64());
        bio = BIO_push(b64, bio);
        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

        char buffer[512];
        int decoded_len = BIO_read(bio, buffer, sizeof(buffer));
        BIO_free_all(bio);

        if (decoded_len > 0) {
            secret_key = std::string(buffer, decoded_len);
        } else {
            secret_key = coinbase_config_.api_secret;
        }
    } else {
        // Advanced Trade Legacy Keys: Use secret directly
        secret_key = coinbase_config_.api_secret;
    }

    // HMAC-SHA256
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         secret_key.c_str(),
         static_cast<int>(secret_key.length()),
         reinterpret_cast<const unsigned char*>(pre_hash.c_str()),
         pre_hash.length(),
         hash, &hash_len);

    if (use_base64_encoding) {
        // Exchange API: Base64 encode the signature
        BIO* b64_out = BIO_new(BIO_f_base64());
        BIO* bio_out = BIO_new(BIO_s_mem());
        bio_out = BIO_push(b64_out, bio_out);

        BIO_set_flags(bio_out, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(bio_out, hash, hash_len);
        BIO_flush(bio_out);

        BUF_MEM* buffer_ptr;
        BIO_get_mem_ptr(bio_out, &buffer_ptr);

        std::string result(buffer_ptr->data, buffer_ptr->length);
        BIO_free_all(bio_out);

        return result;
    } else {
        // Advanced Trade Legacy Keys: Return lowercase hex digest
        std::stringstream ss;
        for (unsigned int i = 0; i < hash_len; ++i) {
            ss << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(hash[i]);
        }
        return ss.str();
    }
}

void CoinbaseExchange::addAuthHeaders(network::HttpRequest& request, const std::string& method,
                                       const std::string& request_path, const std::string& body) {
    std::string timestamp = generateTimestamp();
    std::string signature = sign(timestamp, method, request_path, body);

    request.headers["CB-ACCESS-KEY"] = coinbase_config_.api_key;
    request.headers["CB-ACCESS-SIGN"] = signature;
    request.headers["CB-ACCESS-TIMESTAMP"] = timestamp;

    // Passphrase is required for Exchange API, optional for Advanced Trade API
    if (!coinbase_config_.passphrase.empty()) {
        request.headers["CB-ACCESS-PASSPHRASE"] = coinbase_config_.passphrase;
    }

    request.headers["Content-Type"] = "application/json";
}

std::string CoinbaseExchange::generateJwt() const {
    // Generate JWT for WebSocket authentication (ES256 algorithm)
    // This is required for Advanced Trade WebSocket API
    //
    // JWT Structure:
    // Header: {"alg": "ES256", "kid": "<key_id>", "nonce": "<random>", "typ": "JWT"}
    // Payload: {"iss": "cdp", "nbf": <now>, "exp": <now+120>, "sub": "<api_key>"}
    //
    // Note: For production, you need to sign with EC private key using ES256
    // For legacy keys using HMAC, fall back to the older signature method

    auto now = std::chrono::system_clock::now();
    auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    auto exp_secs = now_secs + 120;  // JWT expires in 2 minutes

    // Generate a random nonce using proper random generator
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    std::stringstream nonce_ss;
    nonce_ss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        nonce_ss << std::setw(2) << dis(gen);
    }
    std::string nonce = nonce_ss.str();

    // For legacy API keys, we use HMAC-based JWT signing
    // For CDP keys (EC private key), ES256 would be used

    // Build JWT header (simplified for legacy keys)
    nlohmann::json header;
    header["alg"] = "HS256";  // HMAC for legacy keys
    header["typ"] = "JWT";
    header["nonce"] = nonce;

    // Build JWT payload
    nlohmann::json payload;
    payload["iss"] = "cdp";
    payload["nbf"] = now_secs;
    payload["exp"] = exp_secs;
    payload["sub"] = coinbase_config_.api_key;

    // Base64url encode header and payload
    auto base64url_encode = [](const std::string& input) -> std::string {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* bio = BIO_new(BIO_s_mem());
        bio = BIO_push(b64, bio);
        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(bio, input.c_str(), input.length());
        BIO_flush(bio);

        BUF_MEM* buffer_ptr;
        BIO_get_mem_ptr(bio, &buffer_ptr);
        std::string result(buffer_ptr->data, buffer_ptr->length);
        BIO_free_all(bio);

        // Convert to base64url (replace + with -, / with _, remove padding =)
        std::replace(result.begin(), result.end(), '+', '-');
        std::replace(result.begin(), result.end(), '/', '_');
        result.erase(std::remove(result.begin(), result.end(), '='), result.end());

        return result;
    };

    std::string header_b64 = base64url_encode(header.dump());
    std::string payload_b64 = base64url_encode(payload.dump());
    std::string unsigned_token = header_b64 + "." + payload_b64;

    // Sign with HMAC-SHA256 for legacy keys
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         coinbase_config_.api_secret.c_str(),
         static_cast<int>(coinbase_config_.api_secret.length()),
         reinterpret_cast<const unsigned char*>(unsigned_token.c_str()),
         unsigned_token.length(),
         hash, &hash_len);

    // Base64url encode signature
    std::string sig_input(reinterpret_cast<char*>(hash), hash_len);
    std::string signature_b64 = base64url_encode(sig_input);

    return unsigned_token + "." + signature_b64;
}

// ============================================================================
// WebSocket Handlers
// ============================================================================

void CoinbaseExchange::handleWsOpen() {
    ws_connected_ = true;
    onConnectionStatus(ConnectionStatus::Connected);

    // Subscribe to heartbeat channel to maintain connection
    // Note: heartbeats channel is only available on the user WebSocket endpoint
    // For market data endpoint, the connection stays alive with regular data flow
    nlohmann::json heartbeat_msg;
    heartbeat_msg["type"] = "subscribe";
    heartbeat_msg["channel"] = "heartbeats";
    heartbeat_msg["product_ids"] = nlohmann::json::array();  // Empty array for heartbeats

    // Add JWT for authenticated connection
    if (!coinbase_config_.api_key.empty()) {
        heartbeat_msg["jwt"] = generateJwt();
    }

    ws_client_->send(heartbeat_msg);
}

void CoinbaseExchange::handleWsClose(int code, const std::string& reason) {
    ws_connected_ = false;
    user_subscribed_ = false;
    onConnectionStatus(ConnectionStatus::Reconnecting);
}

void CoinbaseExchange::handleWsError(const std::string& error) {
    onError("WebSocket error: " + error);
}

void CoinbaseExchange::handleWsMessage(const std::string& message, network::MessageType type) {
    try {
        auto json = nlohmann::json::parse(message);

        std::string channel = json.value("channel", "");
        std::string msg_type = json.value("type", "");

        // Handle sequence numbers for gap detection
        // Sequence numbers are per-channel and should be monotonically increasing
        if (json.contains("sequence_num")) {
            uint64_t seq = json["sequence_num"].get<uint64_t>();
            std::string product_id = "";

            // Extract product_id from events if available
            if (json.contains("events") && !json["events"].empty()) {
                auto& first_event = json["events"][0];
                if (first_event.contains("product_id")) {
                    product_id = first_event["product_id"].get<std::string>();
                }
            }

            if (!product_id.empty()) {
                std::string seq_key = channel + ":" + product_id;
                std::lock_guard<std::mutex> lock(sequence_mutex_);

                auto it = channel_sequences_.find(seq_key);
                if (it != channel_sequences_.end()) {
                    uint64_t expected_seq = it->second + 1;
                    if (seq != expected_seq && seq > it->second) {
                        // Sequence gap detected - may have missed messages
                        onError("Sequence gap detected on " + seq_key +
                               ": expected " + std::to_string(expected_seq) +
                               ", got " + std::to_string(seq));

                        // For orderbook, we should request a new snapshot
                        if (channel == "l2_data" || channel == "level2") {
                            std::lock_guard<std::mutex> ob_lock(orderbook_init_mutex_);
                            orderbook_initialized_[product_id] = false;
                        }
                    }
                }
                channel_sequences_[seq_key] = seq;
            }
        }

        // Handle subscription confirmations
        if (msg_type == "subscriptions") {
            return;
        }

        // Handle errors
        if (msg_type == "error") {
            onError("WebSocket error: " + json.value("message", "unknown"));
            return;
        }

        // Route messages based on channel
        if (channel == "l2_data" || channel == "level2") {
            // Level 2 orderbook data
            auto events = json.value("events", nlohmann::json::array());
            for (const auto& event : events) {
                std::string event_type = event.value("type", "");
                if (event_type == "snapshot") {
                    handleOrderBookSnapshot(event);
                } else if (event_type == "update") {
                    handleOrderBookUpdate(event);
                }
            }
        } else if (channel == "market_trades") {
            // Trade data
            auto events = json.value("events", nlohmann::json::array());
            for (const auto& event : events) {
                handleTradeUpdate(event);
            }
        } else if (channel == "ticker" || channel == "ticker_batch") {
            // Ticker data
            auto events = json.value("events", nlohmann::json::array());
            for (const auto& event : events) {
                handleTickerUpdate(event);
            }
        } else if (channel == "user") {
            // User channel (orders, fills)
            auto events = json.value("events", nlohmann::json::array());
            for (const auto& event : events) {
                handleUserUpdate(event);
            }
        } else if (channel == "heartbeats") {
            // Heartbeat - connection is alive
            return;
        } else if (channel == "status") {
            // Status channel - product/exchange status
            return;
        } else if (channel == "candles") {
            // Candle/OHLC data
            return;
        }

    } catch (const std::exception& e) {
        onError("WS message parse error: " + std::string(e.what()));
    }
}

void CoinbaseExchange::handleOrderBookSnapshot(const nlohmann::json& data) {
    try {
        std::string product_id = data.value("product_id", "");
        if (product_id.empty()) return;

        OrderBook ob;
        ob.symbol = product_id;
        ob.exchange = "coinbase";
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        // Parse updates array for snapshot
        auto updates = data.value("updates", nlohmann::json::array());
        for (const auto& update : updates) {
            std::string side = update.value("side", "");
            double price = std::stod(update.value("price_level", "0"));
            double qty = std::stod(update.value("new_quantity", "0"));

            PriceLevel level;
            level.price = price;
            level.quantity = qty;

            if (side == "bid") {
                ob.bids.push_back(level);
            } else if (side == "offer" || side == "ask") {
                ob.asks.push_back(level);
            }
        }

        // Sort bids descending, asks ascending
        std::sort(ob.bids.begin(), ob.bids.end(),
                  [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
        std::sort(ob.asks.begin(), ob.asks.end(),
                  [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });

        // Mark as initialized
        {
            std::lock_guard<std::mutex> lock(orderbook_init_mutex_);
            orderbook_initialized_[product_id] = true;
        }

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("OrderBook snapshot error: " + std::string(e.what()));
    }
}

void CoinbaseExchange::handleOrderBookUpdate(const nlohmann::json& data) {
    try {
        std::string product_id = data.value("product_id", "");
        if (product_id.empty()) return;

        // Check if orderbook is initialized
        {
            std::lock_guard<std::mutex> lock(orderbook_init_mutex_);
            auto it = orderbook_initialized_.find(product_id);
            if (it == orderbook_initialized_.end() || !it->second) {
                return;  // Wait for snapshot first
            }
        }

        auto cached = getCachedOrderBook(product_id);
        if (!cached) {
            return;
        }

        OrderBook ob = *cached;
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        // Apply updates
        auto updates = data.value("updates", nlohmann::json::array());
        for (const auto& update : updates) {
            std::string side = update.value("side", "");
            double price = std::stod(update.value("price_level", "0"));
            double qty = std::stod(update.value("new_quantity", "0"));

            auto& levels = (side == "bid") ? ob.bids : ob.asks;
            bool ascending = (side != "bid");

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

        onOrderBook(ob);

    } catch (const std::exception& e) {
        onError("OrderBook update error: " + std::string(e.what()));
    }
}

void CoinbaseExchange::handleTradeUpdate(const nlohmann::json& data) {
    try {
        auto trades = data.value("trades", nlohmann::json::array());
        for (const auto& t : trades) {
            Trade trade;
            trade.exchange = "coinbase";
            trade.symbol = t.value("product_id", "");
            trade.trade_id = t.value("trade_id", "");
            trade.price = std::stod(t.value("price", "0"));
            trade.quantity = std::stod(t.value("size", "0"));

            // Parse timestamp
            std::string time_str = t.value("time", "");
            if (!time_str.empty()) {
                // ISO 8601 format, convert to ms
                trade.timestamp = currentTimeMs();  // Simplified
            }
            trade.local_timestamp = currentTimeNs();

            std::string side = t.value("side", "BUY");
            trade.side = (side == "BUY") ? OrderSide::Buy : OrderSide::Sell;

            onTrade(trade);
        }

    } catch (const std::exception& e) {
        onError("Trade update error: " + std::string(e.what()));
    }
}

void CoinbaseExchange::handleTickerUpdate(const nlohmann::json& data) {
    try {
        auto tickers = data.value("tickers", nlohmann::json::array());
        for (const auto& t : tickers) {
            Ticker ticker;
            ticker.exchange = "coinbase";
            ticker.symbol = t.value("product_id", "");
            ticker.last = std::stod(t.value("price", "0"));
            ticker.bid = std::stod(t.value("best_bid", "0"));
            ticker.ask = std::stod(t.value("best_ask", "0"));
            ticker.bid_qty = std::stod(t.value("best_bid_quantity", "0"));
            ticker.ask_qty = std::stod(t.value("best_ask_quantity", "0"));
            ticker.volume_24h = std::stod(t.value("volume_24_h", "0"));
            ticker.high_24h = std::stod(t.value("high_24_h", "0"));
            ticker.low_24h = std::stod(t.value("low_24_h", "0"));
            ticker.change_24h_pct = std::stod(t.value("price_percent_chg_24_h", "0"));
            ticker.timestamp = currentTimeMs();

            onTicker(ticker);
        }

    } catch (const std::exception& e) {
        onError("Ticker update error: " + std::string(e.what()));
    }
}

void CoinbaseExchange::handleUserUpdate(const nlohmann::json& data) {
    try {
        std::string event_type = data.value("type", "");

        if (event_type == "snapshot" || event_type == "update") {
            auto orders = data.value("orders", nlohmann::json::array());
            for (const auto& o : orders) {
                handleOrderUpdate(o);
            }
        }

    } catch (const std::exception& e) {
        onError("User update error: " + std::string(e.what()));
    }
}

void CoinbaseExchange::handleOrderUpdate(const nlohmann::json& data) {
    try {
        Order order;
        order.exchange = "coinbase";
        order.symbol = data.value("product_id", "");
        order.order_id = data.value("order_id", "");
        order.client_order_id = data.value("client_order_id", "");
        order.side = parseOrderSide(data.value("order_side", "BUY"));
        order.type = parseOrderType(data.value("order_type", "LIMIT"));
        order.status = parseOrderStatus(data.value("status", "PENDING"));

        order.quantity = std::stod(data.value("leaves_quantity", "0"));
        order.filled_quantity = std::stod(data.value("cumulative_quantity", "0"));
        order.remaining_quantity = std::stod(data.value("leaves_quantity", "0"));
        order.price = std::stod(data.value("limit_price", "0"));
        order.average_price = std::stod(data.value("average_filled_price", "0"));

        // Parse timestamps
        std::string created = data.value("creation_time", "");
        std::string updated = data.value("last_fill_time", "");
        order.create_time = currentTimeMs();
        order.update_time = currentTimeMs();

        order.raw = data;

        onOrder(order);

    } catch (const std::exception& e) {
        onError("Order update error: " + std::string(e.what()));
    }
}

void CoinbaseExchange::handleMatchUpdate(const nlohmann::json& data) {
    // Handle fill updates - included in order update for Coinbase
}

// ============================================================================
// Subscription Management
// ============================================================================

void CoinbaseExchange::sendSubscribe(const std::vector<std::string>& channels,
                                      const std::vector<std::string>& product_ids) {
    if (!ws_client_ || !ws_connected_.load()) {
        return;
    }

    // Coinbase Advanced Trade WebSocket requires one channel per subscribe message
    for (const auto& channel : channels) {
        nlohmann::json msg;
        msg["type"] = "subscribe";
        msg["product_ids"] = product_ids;
        msg["channel"] = channel;

        // Add JWT authentication for all subscriptions (required for user channel,
        // optional but recommended for public channels)
        bool is_private = (channel == "user" || channel == "futures_balance_summary");

        if (is_private || !coinbase_config_.api_key.empty()) {
            // Use JWT authentication (preferred method for Advanced Trade API)
            std::string jwt = generateJwt();
            msg["jwt"] = jwt;
        }

        ws_client_->send(msg);
    }

    // Track subscriptions
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    for (const auto& channel : channels) {
        subscribed_channels_.insert(channel);
    }
    for (const auto& product : product_ids) {
        subscribed_products_.insert(product);
    }
}

void CoinbaseExchange::sendUnsubscribe(const std::vector<std::string>& channels,
                                        const std::vector<std::string>& product_ids) {
    if (!ws_client_ || !ws_connected_.load()) {
        return;
    }

    // Send unsubscribe for each channel
    for (const auto& channel : channels) {
        nlohmann::json msg;
        msg["type"] = "unsubscribe";
        msg["product_ids"] = product_ids;
        msg["channel"] = channel;

        // Include JWT for authenticated channels
        bool is_private = (channel == "user" || channel == "futures_balance_summary");
        if (is_private && !coinbase_config_.api_key.empty()) {
            msg["jwt"] = generateJwt();
        }

        ws_client_->send(msg);
    }

    // Update subscriptions
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    for (const auto& channel : channels) {
        subscribed_channels_.erase(channel);
    }
}

nlohmann::json CoinbaseExchange::buildAuthMessage() {
    std::string timestamp = generateTimestamp();
    std::string method = "GET";
    std::string path = "/users/self/verify";
    std::string signature = sign(timestamp, method, path, "");

    nlohmann::json auth;
    auth["api_key"] = coinbase_config_.api_key;
    auth["timestamp"] = timestamp;
    auth["signature"] = signature;

    return auth;
}

bool CoinbaseExchange::subscribeOrderBook(const std::string& symbol, int depth) {
    // Channel name is "level2" per Coinbase Advanced Trade WebSocket documentation
    // The response channel will be "l2_data"
    sendSubscribe({"level2"}, {symbol});

    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscribed_products_.insert(symbol);
    return true;
}

bool CoinbaseExchange::unsubscribeOrderBook(const std::string& symbol) {
    sendUnsubscribe({"level2"}, {symbol});

    clearOrderBookCache(symbol);

    std::lock_guard<std::mutex> lock(orderbook_init_mutex_);
    orderbook_initialized_.erase(symbol);

    // Clear sequence tracking for this product
    {
        std::lock_guard<std::mutex> seq_lock(sequence_mutex_);
        channel_sequences_.erase("l2_data:" + symbol);
        channel_sequences_.erase("level2:" + symbol);
    }

    return true;
}

bool CoinbaseExchange::subscribeTrades(const std::string& symbol) {
    sendSubscribe({"market_trades"}, {symbol});
    return true;
}

bool CoinbaseExchange::unsubscribeTrades(const std::string& symbol) {
    sendUnsubscribe({"market_trades"}, {symbol});
    return true;
}

bool CoinbaseExchange::subscribeTicker(const std::string& symbol) {
    sendSubscribe({"ticker"}, {symbol});
    return true;
}

bool CoinbaseExchange::unsubscribeTicker(const std::string& symbol) {
    sendUnsubscribe({"ticker"}, {symbol});
    return true;
}

bool CoinbaseExchange::subscribeUserData() {
    // Get list of subscribed products or use a default
    std::vector<std::string> products;
    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        products = std::vector<std::string>(subscribed_products_.begin(), subscribed_products_.end());
    }

    if (products.empty()) {
        products.push_back("BTC-USD");  // Default product
    }

    sendSubscribe({"user"}, products);
    user_subscribed_ = true;
    return true;
}

bool CoinbaseExchange::unsubscribeUserData() {
    std::vector<std::string> products;
    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        products = std::vector<std::string>(subscribed_products_.begin(), subscribed_products_.end());
    }

    if (!products.empty()) {
        sendUnsubscribe({"user"}, products);
    }
    user_subscribed_ = false;
    return true;
}

// ============================================================================
// Orderbook Management
// ============================================================================

void CoinbaseExchange::initializeOrderBook(const std::string& symbol, int depth) {
    auto snapshot = getOrderBookSnapshot(symbol, depth);
    if (!snapshot) {
        onError("Failed to get orderbook snapshot for " + symbol);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(orderbook_init_mutex_);
        orderbook_initialized_[symbol] = true;
    }

    onOrderBook(*snapshot);
}

void CoinbaseExchange::applyOrderBookDelta(const std::string& symbol, const nlohmann::json& changes) {
    // Handled in handleOrderBookUpdate
}

// ============================================================================
// REST Helpers
// ============================================================================

network::HttpResponse CoinbaseExchange::signedGet(
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

network::HttpResponse CoinbaseExchange::signedPost(
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

network::HttpResponse CoinbaseExchange::signedDelete(
    const std::string& endpoint,
    const nlohmann::json& body) {

    std::string body_str = body.empty() ? "" : body.dump();

    network::HttpRequest request;
    request.method = network::HttpMethod::DELETE;
    request.path = endpoint;
    request.body = body_str;
    addAuthHeaders(request, "DELETE", endpoint, body_str);

    return rest_client_->execute(request);
}

network::HttpResponse CoinbaseExchange::publicGet(
    const std::string& endpoint,
    const std::unordered_map<std::string, std::string>& params) {

    return rest_client_->get(endpoint, params);
}

// ============================================================================
// Order Management
// ============================================================================

std::optional<Order> CoinbaseExchange::placeOrder(const OrderRequest& request) {
    nlohmann::json body;
    body["client_order_id"] = request.client_order_id.empty()
        ? generateClientOrderId()
        : request.client_order_id;
    body["product_id"] = request.symbol;
    body["side"] = orderSideToString(request.side);

    // Build order configuration based on type
    nlohmann::json order_config;

    switch (request.type) {
        case OrderType::Market:
            if (request.side == OrderSide::Buy) {
                // Market buy uses quote_size (amount in quote currency)
                order_config["market_market_ioc"]["quote_size"] = std::to_string(request.quantity * request.price);
            } else {
                // Market sell uses base_size
                order_config["market_market_ioc"]["base_size"] = std::to_string(request.quantity);
            }
            break;

        case OrderType::Limit:
            order_config["limit_limit_gtc"]["base_size"] = std::to_string(request.quantity);
            order_config["limit_limit_gtc"]["limit_price"] = std::to_string(request.price);
            if (request.post_only) {
                order_config["limit_limit_gtc"]["post_only"] = true;
            }
            break;

        case OrderType::StopLimit:
            order_config["stop_limit_stop_limit_gtc"]["base_size"] = std::to_string(request.quantity);
            order_config["stop_limit_stop_limit_gtc"]["limit_price"] = std::to_string(request.price);
            order_config["stop_limit_stop_limit_gtc"]["stop_price"] = std::to_string(request.stop_price);
            order_config["stop_limit_stop_limit_gtc"]["stop_direction"] =
                (request.side == OrderSide::Buy) ? "STOP_DIRECTION_STOP_UP" : "STOP_DIRECTION_STOP_DOWN";
            break;

        case OrderType::ImmediateOrCancel:
            order_config["limit_limit_ioc"]["base_size"] = std::to_string(request.quantity);
            order_config["limit_limit_ioc"]["limit_price"] = std::to_string(request.price);
            break;

        case OrderType::FillOrKill:
            order_config["limit_limit_fok"]["base_size"] = std::to_string(request.quantity);
            order_config["limit_limit_fok"]["limit_price"] = std::to_string(request.price);
            break;

        case OrderType::PostOnly:
            order_config["limit_limit_gtc"]["base_size"] = std::to_string(request.quantity);
            order_config["limit_limit_gtc"]["limit_price"] = std::to_string(request.price);
            order_config["limit_limit_gtc"]["post_only"] = true;
            break;

        default:
            order_config["limit_limit_gtc"]["base_size"] = std::to_string(request.quantity);
            order_config["limit_limit_gtc"]["limit_price"] = std::to_string(request.price);
            break;
    }

    body["order_configuration"] = order_config;

    // Use Advanced Trade API endpoint
    auto response = signedPost("/api/v3/brokerage/orders", body);

    if (!response.success) {
        onError("Place order failed: " + response.error + " " + response.body);
        return std::nullopt;
    }

    try {
        auto json = response.json();

        // Check for errors
        if (json.contains("error")) {
            onError("Place order failed: " + json["error"].dump());
            return std::nullopt;
        }

        // Check success response
        if (!json.value("success", false)) {
            std::string error_msg = json.value("error_response", nlohmann::json{}).value("message", "unknown");
            onError("Place order failed: " + error_msg);
            return std::nullopt;
        }

        auto order_data = json.value("success_response", nlohmann::json{});

        Order order;
        order.exchange = "coinbase";
        order.order_id = order_data.value("order_id", "");
        order.client_order_id = body["client_order_id"];
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

std::optional<Order> CoinbaseExchange::cancelOrder(const std::string& symbol,
                                                    const std::string& order_id) {
    nlohmann::json body;
    body["order_ids"] = nlohmann::json::array({order_id});

    auto response = signedPost("/api/v3/brokerage/orders/batch_cancel", body);

    if (!response.success) {
        onError("Cancel order failed: " + response.error);
        return std::nullopt;
    }

    try {
        auto json = response.json();
        auto results = json.value("results", nlohmann::json::array());

        if (!results.empty() && results[0].value("success", false)) {
            Order order;
            order.order_id = order_id;
            order.symbol = symbol;
            order.status = OrderStatus::Cancelled;
            return order;
        }

        onError("Cancel order failed: " + json.dump());
        return std::nullopt;

    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Order> CoinbaseExchange::cancelOrder(const std::string& symbol,
                                                    const std::string& order_id,
                                                    const std::string& client_order_id) {
    // Coinbase cancels by order_id
    return cancelOrder(symbol, order_id);
}

bool CoinbaseExchange::cancelAllOrders(const std::string& symbol) {
    // Get all open orders first
    auto orders = getOpenOrders(symbol);

    if (orders.empty()) {
        return true;
    }

    // Collect order IDs
    nlohmann::json order_ids = nlohmann::json::array();
    for (const auto& order : orders) {
        order_ids.push_back(order.order_id);
    }

    nlohmann::json body;
    body["order_ids"] = order_ids;

    auto response = signedPost("/api/v3/brokerage/orders/batch_cancel", body);
    return response.success;
}

std::optional<Order> CoinbaseExchange::getOrder(const std::string& symbol,
                                                 const std::string& order_id) {
    auto response = signedGet("/api/v3/brokerage/orders/historical/" + order_id);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        auto order_data = json.value("order", nlohmann::json{});

        if (order_data.empty()) {
            return std::nullopt;
        }

        return parseOrder(order_data);

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Order> CoinbaseExchange::getOpenOrders(const std::string& symbol) {
    std::unordered_map<std::string, std::string> params;
    params["order_status"] = "OPEN";

    if (!symbol.empty()) {
        params["product_id"] = symbol;
    }

    auto response = signedGet("/api/v3/brokerage/orders/historical", params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        auto order_list = json.value("orders", nlohmann::json::array());

        for (const auto& item : order_list) {
            orders.push_back(parseOrder(item));
        }

    } catch (...) {
        // Return empty
    }

    return orders;
}

std::vector<Order> CoinbaseExchange::getOrderHistory(const std::string& symbol,
                                                      uint64_t start_time,
                                                      uint64_t end_time,
                                                      int limit) {
    std::unordered_map<std::string, std::string> params;

    if (!symbol.empty()) {
        params["product_id"] = symbol;
    }

    params["limit"] = std::to_string(limit);

    if (start_time > 0) {
        // Convert to ISO 8601 format
        std::time_t time = start_time / 1000;
        std::tm* gmt = std::gmtime(&time);
        std::ostringstream ss;
        ss << std::put_time(gmt, "%Y-%m-%dT%H:%M:%SZ");
        params["start_date"] = ss.str();
    }

    if (end_time > 0) {
        std::time_t time = end_time / 1000;
        std::tm* gmt = std::gmtime(&time);
        std::ostringstream ss;
        ss << std::put_time(gmt, "%Y-%m-%dT%H:%M:%SZ");
        params["end_date"] = ss.str();
    }

    auto response = signedGet("/api/v3/brokerage/orders/historical", params);

    std::vector<Order> orders;
    if (!response.success) {
        return orders;
    }

    try {
        auto json = response.json();
        auto order_list = json.value("orders", nlohmann::json::array());

        for (const auto& item : order_list) {
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

std::optional<Account> CoinbaseExchange::getAccount() {
    auto response = signedGet("/api/v3/brokerage/accounts");

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();

        Account account;
        account.exchange = "coinbase";
        account.update_time = currentTimeMs();

        auto accounts = json.value("accounts", nlohmann::json::array());
        for (const auto& acc : accounts) {
            Balance balance;

            // Currency can be in "currency" field directly or nested
            if (acc.contains("currency")) {
                balance.asset = acc["currency"].get<std::string>();
            }

            // Parse available_balance - it's an object with "value" and "currency"
            if (acc.contains("available_balance")) {
                auto& avail = acc["available_balance"];
                if (avail.is_object()) {
                    balance.free = std::stod(avail.value("value", "0"));
                    if (balance.asset.empty() && avail.contains("currency")) {
                        balance.asset = avail["currency"].get<std::string>();
                    }
                }
            }

            // Parse hold - also an object with "value" and "currency"
            if (acc.contains("hold")) {
                auto& hold = acc["hold"];
                if (hold.is_object()) {
                    balance.locked = std::stod(hold.value("value", "0"));
                }
            }

            // Calculate total
            balance.total = balance.free + balance.locked;

            if ((balance.free > 0 || balance.locked > 0) && !balance.asset.empty()) {
                account.balances[balance.asset] = balance;
            }
        }

        return account;

    } catch (const std::exception& e) {
        onError("Parse account failed: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<Balance> CoinbaseExchange::getBalance(const std::string& asset) {
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

std::vector<Position> CoinbaseExchange::getPositions(const std::string& symbol) {
    // Coinbase spot does not have positions like futures
    // Return empty vector
    return {};
}

// ============================================================================
// Market Information
// ============================================================================

std::vector<SymbolInfo> CoinbaseExchange::getSymbols() {
    auto response = publicGet("/api/v3/brokerage/products");

    std::vector<SymbolInfo> symbols;
    if (!response.success) {
        return symbols;
    }

    try {
        auto json = response.json();
        auto products = json.value("products", nlohmann::json::array());

        for (const auto& item : products) {
            auto info = parseSymbolInfo(item);
            updateSymbolInfo(info);
            symbols.push_back(info);
        }

    } catch (...) {
        // Return empty
    }

    return symbols;
}

std::optional<SymbolInfo> CoinbaseExchange::getSymbolInfo(const std::string& symbol) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(symbol_info_mutex_);
        auto it = symbol_info_cache_.find(symbol);
        if (it != symbol_info_cache_.end()) {
            return it->second;
        }
    }

    auto response = publicGet("/api/v3/brokerage/products/" + symbol);

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

std::optional<OrderBook> CoinbaseExchange::getOrderBookSnapshot(const std::string& symbol, int depth) {
    std::unordered_map<std::string, std::string> params;
    params["limit"] = std::to_string(depth);

    auto response = publicGet("/api/v3/brokerage/products/" + symbol + "/book", params);

    if (!response.success) {
        return std::nullopt;
    }

    try {
        auto json = response.json();
        auto pricebook = json.value("pricebook", nlohmann::json{});

        OrderBook ob;
        ob.symbol = symbol;
        ob.exchange = "coinbase";
        ob.timestamp = currentTimeMs();
        ob.local_timestamp = currentTimeNs();

        // Parse bids
        auto bids = pricebook.value("bids", nlohmann::json::array());
        for (const auto& b : bids) {
            PriceLevel level;
            level.price = std::stod(b.value("price", "0"));
            level.quantity = std::stod(b.value("size", "0"));
            ob.bids.push_back(level);
        }

        // Parse asks
        auto asks = pricebook.value("asks", nlohmann::json::array());
        for (const auto& a : asks) {
            PriceLevel level;
            level.price = std::stod(a.value("price", "0"));
            level.quantity = std::stod(a.value("size", "0"));
            ob.asks.push_back(level);
        }

        return ob;

    } catch (...) {
        return std::nullopt;
    }
}

std::vector<Trade> CoinbaseExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::unordered_map<std::string, std::string> params;
    params["limit"] = std::to_string(limit);

    auto response = publicGet("/api/v3/brokerage/products/" + symbol + "/ticker", params);

    std::vector<Trade> trades;
    if (!response.success) {
        return trades;
    }

    try {
        auto json = response.json();
        auto trade_list = json.value("trades", nlohmann::json::array());

        for (const auto& t : trade_list) {
            Trade trade;
            trade.exchange = "coinbase";
            trade.symbol = symbol;
            trade.trade_id = t.value("trade_id", "");
            trade.price = std::stod(t.value("price", "0"));
            trade.quantity = std::stod(t.value("size", "0"));
            trade.timestamp = currentTimeMs();
            trade.side = t.value("side", "BUY") == "BUY" ? OrderSide::Buy : OrderSide::Sell;
            trades.push_back(trade);
        }

    } catch (...) {
        // Return empty
    }

    return trades;
}

std::optional<Ticker> CoinbaseExchange::getTicker(const std::string& symbol) {
    // Use the product ticker endpoint for best bid/ask data
    auto response = publicGet("/api/v3/brokerage/products/" + symbol + "/ticker");

    if (!response.success) {
        // Fall back to product info endpoint
        response = publicGet("/api/v3/brokerage/products/" + symbol);
        if (!response.success) {
            return std::nullopt;
        }
    }

    try {
        auto json = response.json();

        Ticker ticker;
        ticker.exchange = "coinbase";
        ticker.symbol = symbol;

        // The ticker endpoint returns different fields than the product endpoint
        if (json.contains("best_bid")) {
            // Ticker endpoint response
            ticker.bid = std::stod(json.value("best_bid", "0"));
            ticker.ask = std::stod(json.value("best_ask", "0"));
            ticker.bid_qty = std::stod(json.value("best_bid_quantity", "0"));
            ticker.ask_qty = std::stod(json.value("best_ask_quantity", "0"));
        }

        // Common fields
        ticker.last = std::stod(json.value("price", "0"));
        ticker.volume_24h = std::stod(json.value("volume_24h", "0"));
        ticker.change_24h_pct = std::stod(json.value("price_percentage_change_24h", "0"));
        ticker.timestamp = currentTimeMs();

        return ticker;

    } catch (...) {
        return std::nullopt;
    }
}

uint64_t CoinbaseExchange::getServerTime() {
    // Coinbase doesn't have a dedicated time endpoint
    // Use current time or infer from response headers
    return currentTimeMs();
}

std::string CoinbaseExchange::formatSymbol(const std::string& base, const std::string& quote) const {
    // Coinbase format: BTC-USD (dash separator)
    return base + "-" + quote;
}

std::pair<std::string, std::string> CoinbaseExchange::parseSymbol(const std::string& symbol) const {
    auto pos = symbol.find('-');
    if (pos != std::string::npos) {
        return {symbol.substr(0, pos), symbol.substr(pos + 1)};
    }
    return {symbol, ""};
}

// ============================================================================
// Data Converters
// ============================================================================

Order CoinbaseExchange::parseOrder(const nlohmann::json& data) {
    Order order;
    order.exchange = "coinbase";
    order.symbol = data.value("product_id", "");
    order.order_id = data.value("order_id", "");
    order.client_order_id = data.value("client_order_id", "");
    order.side = parseOrderSide(data.value("side", "BUY"));
    order.type = parseOrderType(data.value("order_type", "LIMIT"));
    order.status = parseOrderStatus(data.value("status", "PENDING"));

    // Parse order configuration for quantity/price
    auto config = data.value("order_configuration", nlohmann::json{});

    // Try to extract from various order types
    if (config.contains("limit_limit_gtc")) {
        auto limit = config["limit_limit_gtc"];
        order.quantity = std::stod(limit.value("base_size", "0"));
        order.price = std::stod(limit.value("limit_price", "0"));
    } else if (config.contains("limit_limit_ioc")) {
        auto limit = config["limit_limit_ioc"];
        order.quantity = std::stod(limit.value("base_size", "0"));
        order.price = std::stod(limit.value("limit_price", "0"));
    } else if (config.contains("market_market_ioc")) {
        auto market = config["market_market_ioc"];
        order.quantity = std::stod(market.value("base_size", market.value("quote_size", "0")));
    } else if (config.contains("stop_limit_stop_limit_gtc")) {
        auto stop = config["stop_limit_stop_limit_gtc"];
        order.quantity = std::stod(stop.value("base_size", "0"));
        order.price = std::stod(stop.value("limit_price", "0"));
        order.stop_price = std::stod(stop.value("stop_price", "0"));
    }

    order.filled_quantity = std::stod(data.value("filled_size", "0"));
    order.remaining_quantity = order.quantity - order.filled_quantity;
    order.average_price = std::stod(data.value("average_filled_price", "0"));
    order.commission = std::stod(data.value("total_fees", "0"));

    // Parse timestamps
    order.create_time = currentTimeMs();  // Would need ISO parsing
    order.update_time = currentTimeMs();

    order.raw = data;

    return order;
}

OrderStatus CoinbaseExchange::parseOrderStatus(const std::string& status) {
    if (status == "PENDING" || status == "OPEN") return OrderStatus::New;
    if (status == "FILLED") return OrderStatus::Filled;
    if (status == "CANCELLED") return OrderStatus::Cancelled;
    if (status == "EXPIRED") return OrderStatus::Expired;
    if (status == "FAILED") return OrderStatus::Failed;
    if (status == "PENDING_CANCEL") return OrderStatus::PendingCancel;
    return OrderStatus::Failed;
}

OrderSide CoinbaseExchange::parseOrderSide(const std::string& side) {
    return (side == "BUY") ? OrderSide::Buy : OrderSide::Sell;
}

OrderType CoinbaseExchange::parseOrderType(const std::string& type) {
    if (type == "MARKET") return OrderType::Market;
    if (type == "LIMIT") return OrderType::Limit;
    if (type == "STOP" || type == "STOP_LIMIT") return OrderType::StopLimit;
    return OrderType::Limit;
}

TimeInForce CoinbaseExchange::parseTimeInForce(const std::string& tif) {
    if (tif == "GTC" || tif == "GOOD_UNTIL_CANCELLED") return TimeInForce::GTC;
    if (tif == "IOC" || tif == "IMMEDIATE_OR_CANCEL") return TimeInForce::IOC;
    if (tif == "FOK" || tif == "FILL_OR_KILL") return TimeInForce::FOK;
    if (tif == "GTD" || tif == "GOOD_UNTIL_DATE") return TimeInForce::GTD;
    return TimeInForce::GTC;
}

SymbolInfo CoinbaseExchange::parseSymbolInfo(const nlohmann::json& data) {
    SymbolInfo info;
    info.symbol = data.value("product_id", "");
    info.base_asset = data.value("base_currency_id", "");
    info.quote_asset = data.value("quote_currency_id", "");
    info.trading_enabled = data.value("status", "") == "online";

    info.min_qty = std::stod(data.value("base_min_size", "0"));
    info.max_qty = std::stod(data.value("base_max_size", "0"));
    info.step_size = std::stod(data.value("base_increment", "0"));
    info.tick_size = std::stod(data.value("quote_increment", "0"));
    info.min_notional = std::stod(data.value("quote_min_size", "0"));

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

std::string CoinbaseExchange::orderSideToString(OrderSide side) const {
    return side == OrderSide::Buy ? "BUY" : "SELL";
}

std::string CoinbaseExchange::orderTypeToString(OrderType type) const {
    switch (type) {
        case OrderType::Market: return "MARKET";
        case OrderType::Limit: return "LIMIT";
        case OrderType::StopLimit: return "STOP_LIMIT";
        case OrderType::StopMarket: return "STOP";
        default: return "LIMIT";
    }
}

std::string CoinbaseExchange::timeInForceToString(TimeInForce tif) const {
    switch (tif) {
        case TimeInForce::GTC: return "GOOD_UNTIL_CANCELLED";
        case TimeInForce::IOC: return "IMMEDIATE_OR_CANCEL";
        case TimeInForce::FOK: return "FILL_OR_KILL";
        case TimeInForce::GTD: return "GOOD_UNTIL_DATE";
        default: return "GOOD_UNTIL_CANCELLED";
    }
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<CoinbaseExchange> createCoinbaseExchange(const ExchangeConfig& config) {
    return std::make_shared<CoinbaseExchange>(config);
}

std::shared_ptr<CoinbaseExchange> createCoinbaseExchange(const CoinbaseConfig& config) {
    return std::make_shared<CoinbaseExchange>(config);
}

} // namespace exchange
} // namespace hft
