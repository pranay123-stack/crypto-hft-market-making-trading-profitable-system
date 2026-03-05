#pragma once

/**
 * @file exchange_config.hpp
 * @brief Exchange-specific configuration structures and management
 *
 * This module provides type-safe configuration structures for cryptocurrency exchanges.
 * Features:
 * - Exchange-specific configuration structures
 * - API keys and secrets management (with optional encryption)
 * - Rate limits per exchange
 * - WebSocket/REST endpoints
 * - Trading pairs configuration
 *
 * @author HFT System
 * @version 1.0
 */

#include <yaml-cpp/yaml.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <memory>

namespace hft {
namespace config {

/**
 * @brief Supported cryptocurrency exchanges
 */
enum class Exchange : uint8_t {
    BINANCE = 0,
    BYBIT,
    OKX,
    KRAKEN,
    COINBASE,
    KUCOIN,
    GATEIO,
    BITFINEX,
    DERIBIT,
    HTX,
    UNKNOWN
};

/**
 * @brief Convert exchange enum to string
 */
constexpr const char* exchange_to_string(Exchange exchange) {
    switch (exchange) {
        case Exchange::BINANCE:  return "binance";
        case Exchange::BYBIT:    return "bybit";
        case Exchange::OKX:      return "okx";
        case Exchange::KRAKEN:   return "kraken";
        case Exchange::COINBASE: return "coinbase";
        case Exchange::KUCOIN:   return "kucoin";
        case Exchange::GATEIO:   return "gateio";
        case Exchange::BITFINEX: return "bitfinex";
        case Exchange::DERIBIT:  return "deribit";
        case Exchange::HTX:      return "htx";
        default:                 return "unknown";
    }
}

/**
 * @brief Convert string to exchange enum
 */
Exchange string_to_exchange(const std::string& name);

/**
 * @brief API credentials with optional encryption support
 */
struct ApiCredentials {
    std::string api_key;
    std::string api_secret;
    std::string passphrase;           // For exchanges like Coinbase, KuCoin
    std::string subaccount;           // For subaccount trading
    bool encrypted{false};            // Whether credentials are encrypted

    /**
     * @brief Check if credentials are configured
     */
    bool is_valid() const {
        return !api_key.empty() && !api_secret.empty();
    }

    /**
     * @brief Decrypt credentials using provided key
     * @param encryption_key Key used for decryption
     * @return Decrypted credentials
     */
    ApiCredentials decrypt(const std::string& encryption_key) const;
};

/**
 * @brief Rate limit configuration
 */
struct RateLimitConfig {
    uint32_t requests_per_second{10};
    uint32_t requests_per_minute{600};
    uint32_t orders_per_second{10};
    uint32_t orders_per_minute{600};
    uint32_t weight_per_minute{1200};
    uint32_t connection_limit{5};

    // Burst configuration
    uint32_t burst_requests{20};
    std::chrono::milliseconds burst_window{100};
};

/**
 * @brief WebSocket endpoint configuration
 */
struct WebSocketEndpoint {
    std::string url;
    std::string auth_url;                // Separate auth URL if needed
    uint32_t ping_interval_ms{30000};
    uint32_t pong_timeout_ms{10000};
    uint32_t reconnect_delay_ms{1000};
    uint32_t max_reconnect_attempts{10};
    bool compression_enabled{true};
    uint32_t max_message_size{1024 * 1024};  // 1MB default
};

/**
 * @brief REST API endpoint configuration
 */
struct RestEndpoint {
    std::string base_url;
    uint32_t timeout_ms{5000};
    uint32_t connect_timeout_ms{3000};
    bool keep_alive{true};
    uint32_t max_connections{10};
    bool use_http2{true};
};

/**
 * @brief Trading pair configuration
 */
struct TradingPairConfig {
    std::string symbol;                    // Exchange-specific symbol
    std::string base_asset;
    std::string quote_asset;
    std::string normalized_symbol;         // Unified format (e.g., BTC/USDT)

    // Precision settings
    uint8_t price_precision{8};
    uint8_t quantity_precision{8};
    uint8_t quote_precision{8};

    // Size limits
    double min_quantity{0.0};
    double max_quantity{0.0};
    double min_notional{0.0};
    double max_notional{0.0};
    double step_size{0.0};
    double tick_size{0.0};

    // Trading settings
    bool enabled{true};
    bool margin_enabled{false};
    bool spot_enabled{true};
    bool futures_enabled{false};

    // Fees (maker/taker in basis points)
    double maker_fee_bps{10.0};
    double taker_fee_bps{10.0};
};

/**
 * @brief Exchange-specific configuration
 */
struct ExchangeConfig {
    Exchange exchange{Exchange::UNKNOWN};
    std::string name;
    bool enabled{false};
    bool testnet{false};

    // Credentials
    ApiCredentials credentials;
    ApiCredentials testnet_credentials;

    // Endpoints
    WebSocketEndpoint ws_public;
    WebSocketEndpoint ws_private;
    RestEndpoint rest;
    RestEndpoint testnet_rest;
    WebSocketEndpoint testnet_ws_public;
    WebSocketEndpoint testnet_ws_private;

    // Rate limits
    RateLimitConfig rate_limits;

    // Trading pairs
    std::vector<TradingPairConfig> trading_pairs;
    std::unordered_map<std::string, size_t> symbol_index;

    // Exchange-specific settings
    std::unordered_map<std::string, std::string> custom_settings;

    // Timing
    int64_t timestamp_offset_ms{0};        // Server time offset
    bool recv_window_enabled{true};
    uint32_t recv_window_ms{5000};

    /**
     * @brief Get trading pair config by symbol
     */
    const TradingPairConfig* get_trading_pair(const std::string& symbol) const;

    /**
     * @brief Get active credentials (testnet or live)
     */
    const ApiCredentials& get_active_credentials() const;

    /**
     * @brief Get active REST endpoint
     */
    const RestEndpoint& get_active_rest_endpoint() const;

    /**
     * @brief Get active WebSocket endpoint (public)
     */
    const WebSocketEndpoint& get_active_ws_public() const;

    /**
     * @brief Get active WebSocket endpoint (private)
     */
    const WebSocketEndpoint& get_active_ws_private() const;

    /**
     * @brief Check if exchange is properly configured
     */
    bool is_valid() const;
};

/**
 * @brief Exchange configuration manager
 *
 * Manages configuration for all supported exchanges with YAML serialization
 */
class ExchangeConfigManager {
public:
    /**
     * @brief Get singleton instance
     */
    static ExchangeConfigManager& instance();

    // Prevent copying
    ExchangeConfigManager(const ExchangeConfigManager&) = delete;
    ExchangeConfigManager& operator=(const ExchangeConfigManager&) = delete;

    /**
     * @brief Load exchange configurations from YAML file
     * @param filepath Path to YAML configuration file
     */
    void load(const std::string& filepath);

    /**
     * @brief Load exchange configurations from YAML node
     * @param node YAML node containing exchange configurations
     */
    void load_from_node(const YAML::Node& node);

    /**
     * @brief Get configuration for a specific exchange
     * @param exchange Exchange enum
     * @return Pointer to exchange config or nullptr
     */
    const ExchangeConfig* get(Exchange exchange) const;

    /**
     * @brief Get configuration for a specific exchange by name
     * @param name Exchange name
     * @return Pointer to exchange config or nullptr
     */
    const ExchangeConfig* get(const std::string& name) const;

    /**
     * @brief Get all enabled exchanges
     */
    std::vector<Exchange> get_enabled_exchanges() const;

    /**
     * @brief Get all configured exchanges
     */
    std::vector<Exchange> get_all_exchanges() const;

    /**
     * @brief Check if exchange is enabled
     */
    bool is_enabled(Exchange exchange) const;

    /**
     * @brief Set encryption key for credential decryption
     */
    void set_encryption_key(const std::string& key);

    /**
     * @brief Reload configurations
     */
    void reload();

    /**
     * @brief Clear all configurations
     */
    void clear();

private:
    ExchangeConfigManager() = default;

    /**
     * @brief Parse exchange configuration from YAML
     */
    ExchangeConfig parse_exchange_config(const std::string& name, const YAML::Node& node);

    /**
     * @brief Parse API credentials from YAML
     */
    ApiCredentials parse_credentials(const YAML::Node& node);

    /**
     * @brief Parse rate limits from YAML
     */
    RateLimitConfig parse_rate_limits(const YAML::Node& node);

    /**
     * @brief Parse WebSocket endpoint from YAML
     */
    WebSocketEndpoint parse_ws_endpoint(const YAML::Node& node);

    /**
     * @brief Parse REST endpoint from YAML
     */
    RestEndpoint parse_rest_endpoint(const YAML::Node& node);

    /**
     * @brief Parse trading pair configuration from YAML
     */
    TradingPairConfig parse_trading_pair(const YAML::Node& node);

    std::unordered_map<Exchange, ExchangeConfig> configs_;
    std::string encryption_key_;
    std::string config_filepath_;
    mutable std::shared_mutex mutex_;
};

} // namespace config
} // namespace hft

// YAML conversion specializations
namespace YAML {

template<>
struct convert<hft::config::ApiCredentials> {
    static Node encode(const hft::config::ApiCredentials& creds);
    static bool decode(const Node& node, hft::config::ApiCredentials& creds);
};

template<>
struct convert<hft::config::RateLimitConfig> {
    static Node encode(const hft::config::RateLimitConfig& config);
    static bool decode(const Node& node, hft::config::RateLimitConfig& config);
};

template<>
struct convert<hft::config::TradingPairConfig> {
    static Node encode(const hft::config::TradingPairConfig& config);
    static bool decode(const Node& node, hft::config::TradingPairConfig& config);
};

} // namespace YAML
