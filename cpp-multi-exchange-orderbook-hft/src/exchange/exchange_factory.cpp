#include "exchange/exchange_factory.hpp"

#include <algorithm>
#include <stdexcept>

namespace hft {
namespace exchange {

// ============================================================================
// ExchangeFactoryConfig
// ============================================================================

ExchangeFactoryConfig ExchangeFactoryConfig::fromJson(const nlohmann::json& json) {
    ExchangeFactoryConfig config;

    if (json.contains("exchange")) {
        config.exchange_id = stringToExchangeId(json["exchange"].get<std::string>());
    }

    config.api_key = json.value("api_key", "");
    config.api_secret = json.value("api_secret", "");
    config.passphrase = json.value("passphrase", "");
    config.testnet = json.value("testnet", false);
    config.spot = json.value("spot", false);
    config.ws_reconnect_delay_ms = json.value("ws_reconnect_delay_ms", 1000u);
    config.ws_max_reconnect_attempts = json.value("ws_max_reconnect_attempts", 0u);
    config.rest_timeout_ms = json.value("rest_timeout_ms", 10000u);
    config.rest_max_retries = json.value("rest_max_retries", 3u);
    config.orders_per_second = json.value("orders_per_second", 10u);
    config.requests_per_minute = json.value("requests_per_minute", 1200u);

    if (json.contains("extra")) {
        config.extra_params = json["extra"];
    }

    return config;
}

ExchangeConfig ExchangeFactoryConfig::toExchangeConfig() const {
    ExchangeConfig config;
    config.name = exchangeIdToString(exchange_id);
    config.api_key = api_key;
    config.api_secret = api_secret;
    config.passphrase = passphrase;
    config.testnet = testnet;
    config.type = spot ? ExchangeType::Spot : ExchangeType::Perpetual;
    config.ws_reconnect_delay_ms = ws_reconnect_delay_ms;
    config.ws_max_reconnect_attempts = ws_max_reconnect_attempts;
    config.rest_timeout_ms = rest_timeout_ms;
    config.rest_max_retries = rest_max_retries;
    config.orders_per_second = orders_per_second;
    config.requests_per_minute = requests_per_minute;
    config.extra_params = extra_params;
    return config;
}

// ============================================================================
// ExchangeFactory
// ============================================================================

ExchangeFactory& ExchangeFactory::instance() {
    static ExchangeFactory factory;
    return factory;
}

ExchangeFactory::ExchangeFactory() {
    initializeBuiltInCreators();
}

void ExchangeFactory::initializeBuiltInCreators() {
    creators_[ExchangeId::Binance] = createBinanceSpot;
    creators_[ExchangeId::BinanceFutures] = createBinanceFutures;
    creators_[ExchangeId::Bybit] = createBybit;
    creators_[ExchangeId::BybitSpot] = createBybitSpot;
    creators_[ExchangeId::OKX] = createOkx;
    creators_[ExchangeId::OKXSpot] = createOkxSpot;
    creators_[ExchangeId::Kraken] = createKraken;
    creators_[ExchangeId::KrakenFutures] = createKrakenFutures;
    creators_[ExchangeId::Coinbase] = createCoinbase;
    creators_[ExchangeId::KuCoin] = createKuCoin;
    creators_[ExchangeId::GateIO] = createGateIO;
    creators_[ExchangeId::Bitfinex] = createBitfinex;
    creators_[ExchangeId::Deribit] = createDeribit;
    creators_[ExchangeId::HTX] = createHTX;
}

ExchangePtr ExchangeFactory::create(ExchangeId id, const ExchangeFactoryConfig& config) {
    std::lock_guard<std::mutex> lock(creators_mutex_);

    auto it = creators_.find(id);
    if (it == creators_.end()) {
        throw std::runtime_error("Unsupported exchange: " + exchangeIdToString(id));
    }

    return it->second(config);
}

ExchangePtr ExchangeFactory::create(const std::string& name, const ExchangeFactoryConfig& config) {
    // Check custom creators first
    {
        std::lock_guard<std::mutex> lock(creators_mutex_);
        auto it = custom_creators_.find(name);
        if (it != custom_creators_.end()) {
            return it->second(config);
        }
    }

    // Fall back to built-in
    ExchangeId id = stringToExchangeId(name);
    if (id == ExchangeId::Unknown) {
        throw std::runtime_error("Unknown exchange: " + name);
    }

    return create(id, config);
}

ExchangePtr ExchangeFactory::create(const ExchangeFactoryConfig& config) {
    return create(config.exchange_id, config);
}

ExchangePtr ExchangeFactory::createFromJson(const nlohmann::json& json) {
    auto config = ExchangeFactoryConfig::fromJson(json);
    return create(config);
}

std::vector<ExchangePtr> ExchangeFactory::createMultipleFromJson(const nlohmann::json& json) {
    std::vector<ExchangePtr> exchanges;

    if (json.is_array()) {
        for (const auto& item : json) {
            exchanges.push_back(createFromJson(item));
        }
    } else if (json.is_object()) {
        // If object, expect "exchanges" key or treat as single exchange
        if (json.contains("exchanges")) {
            return createMultipleFromJson(json["exchanges"]);
        } else {
            exchanges.push_back(createFromJson(json));
        }
    }

    return exchanges;
}

void ExchangeFactory::registerCreator(ExchangeId id, ExchangeCreator creator) {
    std::lock_guard<std::mutex> lock(creators_mutex_);
    creators_[id] = std::move(creator);
}

void ExchangeFactory::registerCreator(const std::string& name, ExchangeCreator creator) {
    std::lock_guard<std::mutex> lock(creators_mutex_);
    custom_creators_[name] = std::move(creator);
}

bool ExchangeFactory::isSupported(ExchangeId id) const {
    std::lock_guard<std::mutex> lock(creators_mutex_);
    return creators_.find(id) != creators_.end();
}

bool ExchangeFactory::isSupported(const std::string& name) const {
    {
        std::lock_guard<std::mutex> lock(creators_mutex_);
        if (custom_creators_.find(name) != custom_creators_.end()) {
            return true;
        }
    }

    ExchangeId id = stringToExchangeId(name);
    return id != ExchangeId::Unknown && isSupported(id);
}

std::vector<ExchangeId> ExchangeFactory::supportedExchanges() const {
    std::lock_guard<std::mutex> lock(creators_mutex_);

    std::vector<ExchangeId> result;
    result.reserve(creators_.size());

    for (const auto& [id, creator] : creators_) {
        result.push_back(id);
    }

    return result;
}

std::vector<std::string> ExchangeFactory::supportedExchangeNames() const {
    auto ids = supportedExchanges();

    std::vector<std::string> names;
    names.reserve(ids.size());

    for (ExchangeId id : ids) {
        names.push_back(exchangeIdToString(id));
    }

    // Add custom creators
    {
        std::lock_guard<std::mutex> lock(creators_mutex_);
        for (const auto& [name, creator] : custom_creators_) {
            names.push_back(name);
        }
    }

    return names;
}

void ExchangeFactory::cacheInstance(const std::string& key, ExchangePtr exchange) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_instances_[key] = std::move(exchange);
}

ExchangePtr ExchangeFactory::getCachedInstance(const std::string& key) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = cached_instances_.find(key);
    if (it != cached_instances_.end()) {
        return it->second;
    }

    return nullptr;
}

void ExchangeFactory::removeCachedInstance(const std::string& key) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_instances_.erase(key);
}

void ExchangeFactory::clearCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cached_instances_.clear();
}

// ============================================================================
// Built-in Creator Functions
// ============================================================================

ExchangePtr ExchangeFactory::createBinanceSpot(const ExchangeFactoryConfig& config) {
    BinanceConfig binance_config;
    binance_config.api_key = config.api_key;
    binance_config.api_secret = config.api_secret;
    binance_config.testnet = config.testnet;
    binance_config.spot = true;
    binance_config.order_rate_limit = config.orders_per_second;
    binance_config.request_rate_limit = config.requests_per_minute;

    return std::make_shared<BinanceExchange>(binance_config);
}

ExchangePtr ExchangeFactory::createBinanceFutures(const ExchangeFactoryConfig& config) {
    BinanceConfig binance_config;
    binance_config.api_key = config.api_key;
    binance_config.api_secret = config.api_secret;
    binance_config.testnet = config.testnet;
    binance_config.spot = false;
    binance_config.order_rate_limit = config.orders_per_second;
    binance_config.request_rate_limit = config.requests_per_minute;

    return std::make_shared<BinanceExchange>(binance_config);
}

ExchangePtr ExchangeFactory::createBybit(const ExchangeFactoryConfig& config) {
    BybitConfig bybit_config;
    bybit_config.api_key = config.api_key;
    bybit_config.api_secret = config.api_secret;
    bybit_config.testnet = config.testnet;
    bybit_config.category = BybitCategory::Linear;
    bybit_config.order_rate_limit = config.orders_per_second;
    bybit_config.request_rate_limit = config.requests_per_minute;

    return std::make_shared<BybitExchange>(bybit_config);
}

ExchangePtr ExchangeFactory::createBybitSpot(const ExchangeFactoryConfig& config) {
    BybitConfig bybit_config;
    bybit_config.api_key = config.api_key;
    bybit_config.api_secret = config.api_secret;
    bybit_config.testnet = config.testnet;
    bybit_config.category = BybitCategory::Spot;
    bybit_config.order_rate_limit = config.orders_per_second;
    bybit_config.request_rate_limit = config.requests_per_minute;

    return std::make_shared<BybitExchange>(bybit_config);
}

ExchangePtr ExchangeFactory::createOkx(const ExchangeFactoryConfig& config) {
    OkxConfig okx_config;
    okx_config.api_key = config.api_key;
    okx_config.api_secret = config.api_secret;
    okx_config.passphrase = config.passphrase;
    okx_config.testnet = config.testnet;
    okx_config.inst_type = OkxInstType::Swap;
    okx_config.order_rate_limit = config.orders_per_second;
    okx_config.request_rate_limit = config.requests_per_minute;

    return std::make_shared<OkxExchange>(okx_config);
}

ExchangePtr ExchangeFactory::createOkxSpot(const ExchangeFactoryConfig& config) {
    OkxConfig okx_config;
    okx_config.api_key = config.api_key;
    okx_config.api_secret = config.api_secret;
    okx_config.passphrase = config.passphrase;
    okx_config.testnet = config.testnet;
    okx_config.inst_type = OkxInstType::Spot;
    okx_config.order_rate_limit = config.orders_per_second;
    okx_config.request_rate_limit = config.requests_per_minute;

    return std::make_shared<OkxExchange>(okx_config);
}

// Stub implementations for unimplemented exchanges
ExchangePtr ExchangeFactory::createKraken(const ExchangeFactoryConfig& config) {
    throw std::runtime_error("Kraken exchange not yet implemented");
}

ExchangePtr ExchangeFactory::createKrakenFutures(const ExchangeFactoryConfig& config) {
    throw std::runtime_error("Kraken Futures exchange not yet implemented");
}

ExchangePtr ExchangeFactory::createCoinbase(const ExchangeFactoryConfig& config) {
    throw std::runtime_error("Coinbase exchange not yet implemented");
}

ExchangePtr ExchangeFactory::createKuCoin(const ExchangeFactoryConfig& config) {
    KuCoinConfig kucoin_config;
    kucoin_config.api_key = config.api_key;
    kucoin_config.api_secret = config.api_secret;
    kucoin_config.passphrase = config.passphrase;
    kucoin_config.testnet = config.testnet;
    kucoin_config.spot = config.spot;
    kucoin_config.order_rate_limit = config.orders_per_second;
    kucoin_config.request_rate_limit = config.requests_per_minute;

    return std::make_shared<KuCoinExchange>(kucoin_config);
}

ExchangePtr ExchangeFactory::createGateIO(const ExchangeFactoryConfig& config) {
    throw std::runtime_error("Gate.io exchange not yet implemented");
}

ExchangePtr ExchangeFactory::createBitfinex(const ExchangeFactoryConfig& config) {
    throw std::runtime_error("Bitfinex exchange not yet implemented");
}

ExchangePtr ExchangeFactory::createDeribit(const ExchangeFactoryConfig& config) {
    throw std::runtime_error("Deribit exchange not yet implemented");
}

ExchangePtr ExchangeFactory::createHTX(const ExchangeFactoryConfig& config) {
    HTXConfig htx_config;
    htx_config.api_key = config.api_key;
    htx_config.api_secret = config.api_secret;
    htx_config.testnet = config.testnet;
    htx_config.spot = config.spot;
    htx_config.order_rate_limit = config.orders_per_second;
    htx_config.request_rate_limit = config.requests_per_minute;

    return std::make_shared<HTXExchange>(htx_config);
}

// ============================================================================
// Convenience Functions
// ============================================================================

ExchangePtr createExchange(ExchangeId id, const ExchangeFactoryConfig& config) {
    return ExchangeFactory::instance().create(id, config);
}

ExchangePtr createExchange(const std::string& name, const ExchangeFactoryConfig& config) {
    return ExchangeFactory::instance().create(name, config);
}

ExchangePtr createExchangeFromJson(const nlohmann::json& json) {
    return ExchangeFactory::instance().createFromJson(json);
}

} // namespace exchange
} // namespace hft
