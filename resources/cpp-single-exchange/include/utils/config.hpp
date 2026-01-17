#pragma once

#include "core/types.hpp"
#include "strategy/market_maker.hpp"
#include "risk/risk_manager.hpp"
#include "exchange/exchange_client.hpp"
#include <string>
#include <unordered_map>
#include <variant>
#include <optional>

namespace hft {

// ============================================================================
// Configuration Value Types
// ============================================================================

using ConfigValue = std::variant<
    bool,
    int64_t,
    double,
    std::string
>;

// ============================================================================
// Trading Configuration
// ============================================================================

struct TradingConfig {
    // Symbol configuration
    std::string symbol;
    std::string base_asset;
    std::string quote_asset;

    // Precision
    int price_precision = 8;
    int qty_precision = 8;
    Price min_price = 0;
    Price max_price = 0;
    Quantity min_qty = 0;
    Quantity max_qty = 0;
    Quantity step_size = 0;
    Price tick_size = 0;

    // Trading mode
    bool paper_trading = true;
    bool dry_run = false;
};

// ============================================================================
// System Configuration
// ============================================================================

struct SystemConfig {
    // Threading
    int market_data_threads = 1;
    int strategy_threads = 1;
    int order_threads = 1;

    // CPU affinity
    bool cpu_affinity_enabled = false;
    int market_data_cpu = -1;
    int strategy_cpu = -1;
    int order_cpu = -1;

    // Memory
    size_t order_pool_size = 10000;
    size_t tick_buffer_size = 65536;

    // Network
    bool tcp_nodelay = true;
    int recv_buffer_size = 1048576;  // 1MB
    int send_buffer_size = 1048576;

    // Logging
    std::string log_level = "INFO";
    std::string log_dir = "./logs";
    bool log_to_console = true;
    bool log_to_file = true;
};

// ============================================================================
// Full Configuration
// ============================================================================

struct Config {
    TradingConfig trading;
    SystemConfig system;
    ExchangeConfig exchange;
    MarketMakerParams strategy;
    RiskLimits risk;

    // Custom key-value pairs
    std::unordered_map<std::string, ConfigValue> custom;
};

// ============================================================================
// Configuration Parser
// ============================================================================

class ConfigParser {
public:
    // Load from file
    static Config load(const std::string& path);
    static Config load_json(const std::string& path);
    static Config load_yaml(const std::string& path);
    static Config load_toml(const std::string& path);

    // Load from string
    static Config parse_json(const std::string& json);
    static Config parse_yaml(const std::string& yaml);

    // Save to file
    static void save(const Config& config, const std::string& path);
    static void save_json(const Config& config, const std::string& path);

    // Environment variable override
    static void apply_env_overrides(Config& config);

    // Validation
    static bool validate(const Config& config, std::string& error);
};

// ============================================================================
// Configuration Builder
// ============================================================================

class ConfigBuilder {
public:
    ConfigBuilder() = default;

    // Exchange settings
    ConfigBuilder& exchange(const std::string& name);
    ConfigBuilder& api_key(const std::string& key);
    ConfigBuilder& api_secret(const std::string& secret);
    ConfigBuilder& testnet(bool enabled = true);

    // Trading settings
    ConfigBuilder& symbol(const std::string& symbol);
    ConfigBuilder& paper_trading(bool enabled = true);

    // Strategy settings
    ConfigBuilder& spread_bps(double min, double max, double target);
    ConfigBuilder& order_size(Quantity default_size, Quantity min, Quantity max);
    ConfigBuilder& max_position(Quantity max);
    ConfigBuilder& inventory_skew(double skew);

    // Risk settings
    ConfigBuilder& max_daily_loss(double amount);
    ConfigBuilder& max_drawdown(double percent);
    ConfigBuilder& rate_limit(uint32_t orders_per_second);

    // System settings
    ConfigBuilder& log_level(const std::string& level);
    ConfigBuilder& cpu_affinity(int md_cpu, int strategy_cpu, int order_cpu);

    // Build
    [[nodiscard]] Config build() const;

private:
    Config config_;
};

// ============================================================================
// Default Configurations
// ============================================================================

namespace defaults {

inline Config binance_spot_config() {
    return ConfigBuilder()
        .exchange("binance")
        .testnet(true)
        .symbol("BTCUSDT")
        .paper_trading(true)
        .spread_bps(5.0, 50.0, 10.0)
        .order_size(to_qty(0.001), to_qty(0.0001), to_qty(0.1))
        .max_position(to_qty(1.0))
        .max_daily_loss(100.0)
        .rate_limit(10)
        .log_level("INFO")
        .build();
}

inline Config binance_futures_config() {
    auto config = binance_spot_config();
    // Futures-specific settings would go here
    return config;
}

} // namespace defaults

} // namespace hft
