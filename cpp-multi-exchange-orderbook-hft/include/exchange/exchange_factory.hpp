#pragma once

#include "exchange/exchange_base.hpp"
#include "exchange/binance.hpp"
#include "exchange/bybit.hpp"
#include "exchange/okx.hpp"
#include "exchange/kucoin.hpp"
#include "exchange/htx.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <vector>

namespace hft {
namespace exchange {

// Supported exchanges
enum class ExchangeId {
    Binance,
    BinanceFutures,
    Bybit,
    BybitSpot,
    OKX,
    OKXSpot,
    Kraken,
    KrakenFutures,
    Coinbase,
    KuCoin,
    GateIO,
    Bitfinex,
    Deribit,
    HTX,
    Unknown
};

// Convert exchange ID to string
inline std::string exchangeIdToString(ExchangeId id) {
    switch (id) {
        case ExchangeId::Binance: return "binance";
        case ExchangeId::BinanceFutures: return "binance_futures";
        case ExchangeId::Bybit: return "bybit";
        case ExchangeId::BybitSpot: return "bybit_spot";
        case ExchangeId::OKX: return "okx";
        case ExchangeId::OKXSpot: return "okx_spot";
        case ExchangeId::Kraken: return "kraken";
        case ExchangeId::KrakenFutures: return "kraken_futures";
        case ExchangeId::Coinbase: return "coinbase";
        case ExchangeId::KuCoin: return "kucoin";
        case ExchangeId::GateIO: return "gateio";
        case ExchangeId::Bitfinex: return "bitfinex";
        case ExchangeId::Deribit: return "deribit";
        case ExchangeId::HTX: return "htx";
        default: return "unknown";
    }
}

// Convert string to exchange ID
inline ExchangeId stringToExchangeId(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "binance") return ExchangeId::Binance;
    if (lower == "binance_futures" || lower == "binancefutures") return ExchangeId::BinanceFutures;
    if (lower == "bybit") return ExchangeId::Bybit;
    if (lower == "bybit_spot" || lower == "bybitspot") return ExchangeId::BybitSpot;
    if (lower == "okx") return ExchangeId::OKX;
    if (lower == "okx_spot" || lower == "okxspot") return ExchangeId::OKXSpot;
    if (lower == "kraken") return ExchangeId::Kraken;
    if (lower == "kraken_futures" || lower == "krakenfutures") return ExchangeId::KrakenFutures;
    if (lower == "coinbase") return ExchangeId::Coinbase;
    if (lower == "kucoin") return ExchangeId::KuCoin;
    if (lower == "gateio" || lower == "gate") return ExchangeId::GateIO;
    if (lower == "bitfinex") return ExchangeId::Bitfinex;
    if (lower == "deribit") return ExchangeId::Deribit;
    if (lower == "htx" || lower == "huobi") return ExchangeId::HTX;

    return ExchangeId::Unknown;
}

// Exchange factory configuration (from JSON/YAML)
struct ExchangeFactoryConfig {
    ExchangeId exchange_id{ExchangeId::Unknown};
    std::string api_key;
    std::string api_secret;
    std::string passphrase;       // For OKX, etc.
    bool testnet{false};
    bool spot{false};             // Spot or derivatives
    uint32_t ws_reconnect_delay_ms{1000};
    uint32_t ws_max_reconnect_attempts{0};
    uint32_t rest_timeout_ms{10000};
    uint32_t rest_max_retries{3};
    uint32_t orders_per_second{10};
    uint32_t requests_per_minute{1200};
    nlohmann::json extra_params;

    // Load from JSON
    static ExchangeFactoryConfig fromJson(const nlohmann::json& json);

    // Convert to ExchangeConfig
    ExchangeConfig toExchangeConfig() const;
};

// Exchange creation function type
using ExchangeCreator = std::function<ExchangePtr(const ExchangeFactoryConfig&)>;

// Exchange factory - singleton pattern for managing exchange instances
class ExchangeFactory {
public:
    // Get singleton instance
    static ExchangeFactory& instance();

    // Create a new exchange instance
    ExchangePtr create(ExchangeId id, const ExchangeFactoryConfig& config);
    ExchangePtr create(const std::string& name, const ExchangeFactoryConfig& config);
    ExchangePtr create(const ExchangeFactoryConfig& config);

    // Create from JSON configuration
    ExchangePtr createFromJson(const nlohmann::json& json);

    // Create multiple exchanges from JSON array
    std::vector<ExchangePtr> createMultipleFromJson(const nlohmann::json& json);

    // Register a custom exchange creator
    void registerCreator(ExchangeId id, ExchangeCreator creator);
    void registerCreator(const std::string& name, ExchangeCreator creator);

    // Check if exchange is supported
    bool isSupported(ExchangeId id) const;
    bool isSupported(const std::string& name) const;

    // Get list of supported exchanges
    std::vector<ExchangeId> supportedExchanges() const;
    std::vector<std::string> supportedExchangeNames() const;

    // Manage exchange instances (for singleton exchanges)
    void cacheInstance(const std::string& key, ExchangePtr exchange);
    ExchangePtr getCachedInstance(const std::string& key);
    void removeCachedInstance(const std::string& key);
    void clearCache();

private:
    ExchangeFactory();
    ~ExchangeFactory() = default;

    // Non-copyable
    ExchangeFactory(const ExchangeFactory&) = delete;
    ExchangeFactory& operator=(const ExchangeFactory&) = delete;

    // Initialize built-in creators
    void initializeBuiltInCreators();

    // Built-in creator functions
    static ExchangePtr createBinanceSpot(const ExchangeFactoryConfig& config);
    static ExchangePtr createBinanceFutures(const ExchangeFactoryConfig& config);
    static ExchangePtr createBybit(const ExchangeFactoryConfig& config);
    static ExchangePtr createBybitSpot(const ExchangeFactoryConfig& config);
    static ExchangePtr createOkx(const ExchangeFactoryConfig& config);
    static ExchangePtr createOkxSpot(const ExchangeFactoryConfig& config);

    // Stub creators for unimplemented exchanges
    static ExchangePtr createKraken(const ExchangeFactoryConfig& config);
    static ExchangePtr createKrakenFutures(const ExchangeFactoryConfig& config);
    static ExchangePtr createCoinbase(const ExchangeFactoryConfig& config);
    static ExchangePtr createKuCoin(const ExchangeFactoryConfig& config);
    static ExchangePtr createGateIO(const ExchangeFactoryConfig& config);
    static ExchangePtr createBitfinex(const ExchangeFactoryConfig& config);
    static ExchangePtr createDeribit(const ExchangeFactoryConfig& config);
    static ExchangePtr createHTX(const ExchangeFactoryConfig& config);

    // Registered creators
    std::unordered_map<ExchangeId, ExchangeCreator> creators_;
    std::unordered_map<std::string, ExchangeCreator> custom_creators_;

    // Cached instances
    std::unordered_map<std::string, ExchangePtr> cached_instances_;
    mutable std::mutex cache_mutex_;
    mutable std::mutex creators_mutex_;
};

// Convenience functions
ExchangePtr createExchange(ExchangeId id, const ExchangeFactoryConfig& config);
ExchangePtr createExchange(const std::string& name, const ExchangeFactoryConfig& config);
ExchangePtr createExchangeFromJson(const nlohmann::json& json);

} // namespace exchange
} // namespace hft
