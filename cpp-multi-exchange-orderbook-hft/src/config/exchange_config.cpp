/**
 * @file exchange_config.cpp
 * @brief Implementation of exchange-specific configuration management
 */

#include "config/exchange_config.hpp"
#include <algorithm>
#include <fstream>
#include <shared_mutex>

namespace hft {
namespace config {

// ============================================================================
// Exchange Conversion
// ============================================================================

Exchange string_to_exchange(const std::string& name) {
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

    if (lower_name == "binance") return Exchange::BINANCE;
    if (lower_name == "bybit") return Exchange::BYBIT;
    if (lower_name == "okx") return Exchange::OKX;
    if (lower_name == "kraken") return Exchange::KRAKEN;
    if (lower_name == "coinbase") return Exchange::COINBASE;
    if (lower_name == "kucoin") return Exchange::KUCOIN;
    if (lower_name == "gateio" || lower_name == "gate.io" || lower_name == "gate") return Exchange::GATEIO;
    if (lower_name == "bitfinex") return Exchange::BITFINEX;
    if (lower_name == "deribit") return Exchange::DERIBIT;
    if (lower_name == "htx" || lower_name == "huobi") return Exchange::HTX;

    return Exchange::UNKNOWN;
}

// ============================================================================
// ApiCredentials
// ============================================================================

ApiCredentials ApiCredentials::decrypt(const std::string& encryption_key) const {
    if (!encrypted || encryption_key.empty()) {
        return *this;
    }

    ApiCredentials decrypted = *this;
    // In production, implement proper AES-256-GCM decryption
    // For now, return as-is (credentials should be encrypted at rest)
    decrypted.encrypted = false;
    return decrypted;
}

// ============================================================================
// ExchangeConfig
// ============================================================================

const TradingPairConfig* ExchangeConfig::get_trading_pair(const std::string& symbol) const {
    auto it = symbol_index.find(symbol);
    if (it != symbol_index.end() && it->second < trading_pairs.size()) {
        return &trading_pairs[it->second];
    }
    return nullptr;
}

const ApiCredentials& ExchangeConfig::get_active_credentials() const {
    return testnet ? testnet_credentials : credentials;
}

const RestEndpoint& ExchangeConfig::get_active_rest_endpoint() const {
    return testnet ? testnet_rest : rest;
}

const WebSocketEndpoint& ExchangeConfig::get_active_ws_public() const {
    return testnet ? testnet_ws_public : ws_public;
}

const WebSocketEndpoint& ExchangeConfig::get_active_ws_private() const {
    return testnet ? testnet_ws_private : ws_private;
}

bool ExchangeConfig::is_valid() const {
    if (!enabled) return true;  // Disabled configs don't need to be valid

    // Check credentials
    const auto& creds = get_active_credentials();
    if (!creds.is_valid()) {
        return false;
    }

    // Check endpoints
    const auto& rest_ep = get_active_rest_endpoint();
    if (rest_ep.base_url.empty()) {
        return false;
    }

    // Check at least one trading pair
    if (trading_pairs.empty()) {
        return false;
    }

    return true;
}

// ============================================================================
// ExchangeConfigManager
// ============================================================================

ExchangeConfigManager& ExchangeConfigManager::instance() {
    static ExchangeConfigManager instance;
    return instance;
}

void ExchangeConfigManager::load(const std::string& filepath) {
    config_filepath_ = filepath;

    YAML::Node root = YAML::LoadFile(filepath);

    std::unique_lock lock(mutex_);
    configs_.clear();

    // Parse exchanges section
    if (root["exchanges"]) {
        load_from_node(root["exchanges"]);
    } else {
        load_from_node(root);
    }
}

void ExchangeConfigManager::load_from_node(const YAML::Node& node) {
    if (!node.IsMap()) {
        return;
    }

    for (const auto& kv : node) {
        std::string name = kv.first.as<std::string>();
        Exchange exchange = string_to_exchange(name);

        if (exchange == Exchange::UNKNOWN) {
            continue;  // Skip unknown exchanges
        }

        ExchangeConfig config = parse_exchange_config(name, kv.second);
        config.exchange = exchange;
        config.name = name;

        // Build symbol index
        for (size_t i = 0; i < config.trading_pairs.size(); ++i) {
            config.symbol_index[config.trading_pairs[i].symbol] = i;
            config.symbol_index[config.trading_pairs[i].normalized_symbol] = i;
        }

        configs_[exchange] = std::move(config);
    }
}

ExchangeConfig ExchangeConfigManager::parse_exchange_config(
    const std::string& name, const YAML::Node& node) {

    ExchangeConfig config;

    config.enabled = node["enabled"].as<bool>(false);
    config.testnet = node["testnet"].as<bool>(false);

    // Parse credentials
    if (node["api_key"] || node["credentials"]) {
        config.credentials = parse_credentials(node["credentials"] ? node["credentials"] : node);
    }

    if (node["testnet_credentials"]) {
        config.testnet_credentials = parse_credentials(node["testnet_credentials"]);
    }

    // Parse rate limits
    if (node["rate_limits"]) {
        config.rate_limits = parse_rate_limits(node["rate_limits"]);
    }

    // Parse endpoints
    if (node["endpoints"]) {
        auto endpoints = node["endpoints"];

        if (endpoints["rest"]) {
            config.rest = parse_rest_endpoint(endpoints["rest"]);
        }
        if (endpoints["ws_public"]) {
            config.ws_public = parse_ws_endpoint(endpoints["ws_public"]);
        }
        if (endpoints["ws_private"]) {
            config.ws_private = parse_ws_endpoint(endpoints["ws_private"]);
        }
        if (endpoints["testnet_rest"]) {
            config.testnet_rest = parse_rest_endpoint(endpoints["testnet_rest"]);
        }
        if (endpoints["testnet_ws_public"]) {
            config.testnet_ws_public = parse_ws_endpoint(endpoints["testnet_ws_public"]);
        }
        if (endpoints["testnet_ws_private"]) {
            config.testnet_ws_private = parse_ws_endpoint(endpoints["testnet_ws_private"]);
        }
    }

    // Parse trading pairs
    if (node["trading_pairs"]) {
        for (const auto& pair_node : node["trading_pairs"]) {
            config.trading_pairs.push_back(parse_trading_pair(pair_node));
        }
    }

    // Parse custom settings
    if (node["custom"]) {
        for (const auto& kv : node["custom"]) {
            config.custom_settings[kv.first.as<std::string>()] = kv.second.as<std::string>();
        }
    }

    // Parse timing settings
    config.timestamp_offset_ms = node["timestamp_offset_ms"].as<int64_t>(0);
    config.recv_window_enabled = node["recv_window_enabled"].as<bool>(true);
    config.recv_window_ms = node["recv_window_ms"].as<uint32_t>(5000);

    return config;
}

ApiCredentials ExchangeConfigManager::parse_credentials(const YAML::Node& node) {
    ApiCredentials creds;

    creds.api_key = node["api_key"].as<std::string>("");
    creds.api_secret = node["api_secret"].as<std::string>("");
    creds.passphrase = node["passphrase"].as<std::string>("");
    creds.subaccount = node["subaccount"].as<std::string>("");
    creds.encrypted = node["encrypted"].as<bool>(false);

    // Decrypt if encryption key is set
    if (creds.encrypted && !encryption_key_.empty()) {
        creds = creds.decrypt(encryption_key_);
    }

    return creds;
}

RateLimitConfig ExchangeConfigManager::parse_rate_limits(const YAML::Node& node) {
    RateLimitConfig limits;

    limits.requests_per_second = node["requests_per_second"].as<uint32_t>(10);
    limits.requests_per_minute = node["requests_per_minute"].as<uint32_t>(600);
    limits.orders_per_second = node["orders_per_second"].as<uint32_t>(10);
    limits.orders_per_minute = node["orders_per_minute"].as<uint32_t>(600);
    limits.weight_per_minute = node["weight_per_minute"].as<uint32_t>(1200);
    limits.connection_limit = node["connection_limit"].as<uint32_t>(5);
    limits.burst_requests = node["burst_requests"].as<uint32_t>(20);
    limits.burst_window = std::chrono::milliseconds(node["burst_window_ms"].as<uint32_t>(100));

    return limits;
}

WebSocketEndpoint ExchangeConfigManager::parse_ws_endpoint(const YAML::Node& node) {
    WebSocketEndpoint endpoint;

    if (node.IsScalar()) {
        endpoint.url = node.as<std::string>();
        return endpoint;
    }

    endpoint.url = node["url"].as<std::string>("");
    endpoint.auth_url = node["auth_url"].as<std::string>("");
    endpoint.ping_interval_ms = node["ping_interval_ms"].as<uint32_t>(30000);
    endpoint.pong_timeout_ms = node["pong_timeout_ms"].as<uint32_t>(10000);
    endpoint.reconnect_delay_ms = node["reconnect_delay_ms"].as<uint32_t>(1000);
    endpoint.max_reconnect_attempts = node["max_reconnect_attempts"].as<uint32_t>(10);
    endpoint.compression_enabled = node["compression_enabled"].as<bool>(true);
    endpoint.max_message_size = node["max_message_size"].as<uint32_t>(1024 * 1024);

    return endpoint;
}

RestEndpoint ExchangeConfigManager::parse_rest_endpoint(const YAML::Node& node) {
    RestEndpoint endpoint;

    if (node.IsScalar()) {
        endpoint.base_url = node.as<std::string>();
        return endpoint;
    }

    endpoint.base_url = node["base_url"].as<std::string>("");
    endpoint.timeout_ms = node["timeout_ms"].as<uint32_t>(5000);
    endpoint.connect_timeout_ms = node["connect_timeout_ms"].as<uint32_t>(3000);
    endpoint.keep_alive = node["keep_alive"].as<bool>(true);
    endpoint.max_connections = node["max_connections"].as<uint32_t>(10);
    endpoint.use_http2 = node["use_http2"].as<bool>(true);

    return endpoint;
}

TradingPairConfig ExchangeConfigManager::parse_trading_pair(const YAML::Node& node) {
    TradingPairConfig pair;

    pair.symbol = node["symbol"].as<std::string>("");
    pair.base_asset = node["base_asset"].as<std::string>("");
    pair.quote_asset = node["quote_asset"].as<std::string>("");
    pair.normalized_symbol = node["normalized_symbol"].as<std::string>(
        pair.base_asset + "/" + pair.quote_asset);

    pair.price_precision = node["price_precision"].as<uint8_t>(8);
    pair.quantity_precision = node["quantity_precision"].as<uint8_t>(8);
    pair.quote_precision = node["quote_precision"].as<uint8_t>(8);

    pair.min_quantity = node["min_quantity"].as<double>(0.0);
    pair.max_quantity = node["max_quantity"].as<double>(0.0);
    pair.min_notional = node["min_notional"].as<double>(0.0);
    pair.max_notional = node["max_notional"].as<double>(0.0);
    pair.step_size = node["step_size"].as<double>(0.0);
    pair.tick_size = node["tick_size"].as<double>(0.0);

    pair.enabled = node["enabled"].as<bool>(true);
    pair.margin_enabled = node["margin_enabled"].as<bool>(false);
    pair.spot_enabled = node["spot_enabled"].as<bool>(true);
    pair.futures_enabled = node["futures_enabled"].as<bool>(false);

    pair.maker_fee_bps = node["maker_fee_bps"].as<double>(10.0);
    pair.taker_fee_bps = node["taker_fee_bps"].as<double>(10.0);

    return pair;
}

const ExchangeConfig* ExchangeConfigManager::get(Exchange exchange) const {
    std::shared_lock lock(mutex_);
    auto it = configs_.find(exchange);
    return it != configs_.end() ? &it->second : nullptr;
}

const ExchangeConfig* ExchangeConfigManager::get(const std::string& name) const {
    return get(string_to_exchange(name));
}

std::vector<Exchange> ExchangeConfigManager::get_enabled_exchanges() const {
    std::shared_lock lock(mutex_);
    std::vector<Exchange> enabled;

    for (const auto& [exchange, config] : configs_) {
        if (config.enabled) {
            enabled.push_back(exchange);
        }
    }

    return enabled;
}

std::vector<Exchange> ExchangeConfigManager::get_all_exchanges() const {
    std::shared_lock lock(mutex_);
    std::vector<Exchange> exchanges;

    for (const auto& [exchange, config] : configs_) {
        exchanges.push_back(exchange);
    }

    return exchanges;
}

bool ExchangeConfigManager::is_enabled(Exchange exchange) const {
    auto config = get(exchange);
    return config && config->enabled;
}

void ExchangeConfigManager::set_encryption_key(const std::string& key) {
    std::unique_lock lock(mutex_);
    encryption_key_ = key;
}

void ExchangeConfigManager::reload() {
    if (!config_filepath_.empty()) {
        load(config_filepath_);
    }
}

void ExchangeConfigManager::clear() {
    std::unique_lock lock(mutex_);
    configs_.clear();
}

} // namespace config
} // namespace hft

// ============================================================================
// YAML Conversion Implementations
// ============================================================================

namespace YAML {

Node convert<hft::config::ApiCredentials>::encode(const hft::config::ApiCredentials& creds) {
    Node node;
    node["api_key"] = creds.api_key;
    node["api_secret"] = creds.api_secret;
    if (!creds.passphrase.empty()) {
        node["passphrase"] = creds.passphrase;
    }
    if (!creds.subaccount.empty()) {
        node["subaccount"] = creds.subaccount;
    }
    node["encrypted"] = creds.encrypted;
    return node;
}

bool convert<hft::config::ApiCredentials>::decode(const Node& node, hft::config::ApiCredentials& creds) {
    if (!node.IsMap()) {
        return false;
    }

    creds.api_key = node["api_key"].as<std::string>("");
    creds.api_secret = node["api_secret"].as<std::string>("");
    creds.passphrase = node["passphrase"].as<std::string>("");
    creds.subaccount = node["subaccount"].as<std::string>("");
    creds.encrypted = node["encrypted"].as<bool>(false);

    return true;
}

Node convert<hft::config::RateLimitConfig>::encode(const hft::config::RateLimitConfig& config) {
    Node node;
    node["requests_per_second"] = config.requests_per_second;
    node["requests_per_minute"] = config.requests_per_minute;
    node["orders_per_second"] = config.orders_per_second;
    node["orders_per_minute"] = config.orders_per_minute;
    node["weight_per_minute"] = config.weight_per_minute;
    node["connection_limit"] = config.connection_limit;
    node["burst_requests"] = config.burst_requests;
    node["burst_window_ms"] = static_cast<uint32_t>(config.burst_window.count());
    return node;
}

bool convert<hft::config::RateLimitConfig>::decode(const Node& node, hft::config::RateLimitConfig& config) {
    if (!node.IsMap()) {
        return false;
    }

    config.requests_per_second = node["requests_per_second"].as<uint32_t>(10);
    config.requests_per_minute = node["requests_per_minute"].as<uint32_t>(600);
    config.orders_per_second = node["orders_per_second"].as<uint32_t>(10);
    config.orders_per_minute = node["orders_per_minute"].as<uint32_t>(600);
    config.weight_per_minute = node["weight_per_minute"].as<uint32_t>(1200);
    config.connection_limit = node["connection_limit"].as<uint32_t>(5);
    config.burst_requests = node["burst_requests"].as<uint32_t>(20);
    config.burst_window = std::chrono::milliseconds(node["burst_window_ms"].as<uint32_t>(100));

    return true;
}

Node convert<hft::config::TradingPairConfig>::encode(const hft::config::TradingPairConfig& config) {
    Node node;
    node["symbol"] = config.symbol;
    node["base_asset"] = config.base_asset;
    node["quote_asset"] = config.quote_asset;
    node["normalized_symbol"] = config.normalized_symbol;
    node["price_precision"] = config.price_precision;
    node["quantity_precision"] = config.quantity_precision;
    node["quote_precision"] = config.quote_precision;
    node["min_quantity"] = config.min_quantity;
    node["max_quantity"] = config.max_quantity;
    node["min_notional"] = config.min_notional;
    node["max_notional"] = config.max_notional;
    node["step_size"] = config.step_size;
    node["tick_size"] = config.tick_size;
    node["enabled"] = config.enabled;
    node["margin_enabled"] = config.margin_enabled;
    node["spot_enabled"] = config.spot_enabled;
    node["futures_enabled"] = config.futures_enabled;
    node["maker_fee_bps"] = config.maker_fee_bps;
    node["taker_fee_bps"] = config.taker_fee_bps;
    return node;
}

bool convert<hft::config::TradingPairConfig>::decode(const Node& node, hft::config::TradingPairConfig& config) {
    if (!node.IsMap()) {
        return false;
    }

    config.symbol = node["symbol"].as<std::string>("");
    config.base_asset = node["base_asset"].as<std::string>("");
    config.quote_asset = node["quote_asset"].as<std::string>("");
    config.normalized_symbol = node["normalized_symbol"].as<std::string>(
        config.base_asset + "/" + config.quote_asset);
    config.price_precision = node["price_precision"].as<uint8_t>(8);
    config.quantity_precision = node["quantity_precision"].as<uint8_t>(8);
    config.quote_precision = node["quote_precision"].as<uint8_t>(8);
    config.min_quantity = node["min_quantity"].as<double>(0.0);
    config.max_quantity = node["max_quantity"].as<double>(0.0);
    config.min_notional = node["min_notional"].as<double>(0.0);
    config.max_notional = node["max_notional"].as<double>(0.0);
    config.step_size = node["step_size"].as<double>(0.0);
    config.tick_size = node["tick_size"].as<double>(0.0);
    config.enabled = node["enabled"].as<bool>(true);
    config.margin_enabled = node["margin_enabled"].as<bool>(false);
    config.spot_enabled = node["spot_enabled"].as<bool>(true);
    config.futures_enabled = node["futures_enabled"].as<bool>(false);
    config.maker_fee_bps = node["maker_fee_bps"].as<double>(10.0);
    config.taker_fee_bps = node["taker_fee_bps"].as<double>(10.0);

    return true;
}

} // namespace YAML
