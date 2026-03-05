/**
 * @file strategy_config.cpp
 * @brief Implementation of strategy configuration management
 */

#include "config/strategy_config.hpp"
#include <algorithm>
#include <fstream>

namespace hft {
namespace config {

// ============================================================================
// Strategy Type Conversion
// ============================================================================

StrategyType string_to_strategy_type(const std::string& name) {
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

    // HFT Strategies
    if (lower_name == "latency_arbitrage" || lower_name == "latencyarbitrage") {
        return StrategyType::LATENCY_ARBITRAGE;
    }
    if (lower_name == "triangular_arbitrage" || lower_name == "triangulararbitrage") {
        return StrategyType::TRIANGULAR_ARBITRAGE;
    }
    if (lower_name == "statistical_arbitrage" || lower_name == "statisticalarbitrage" ||
        lower_name == "stat_arb" || lower_name == "statarb") {
        return StrategyType::STATISTICAL_ARBITRAGE;
    }
    if (lower_name == "order_flow_imbalance" || lower_name == "orderflowimbalance" ||
        lower_name == "order_flow") {
        return StrategyType::ORDER_FLOW_IMBALANCE;
    }
    if (lower_name == "microstructure_alpha" || lower_name == "microstructurealpha" ||
        lower_name == "microstructure") {
        return StrategyType::MICROSTRUCTURE_ALPHA;
    }

    // Market Making Strategies
    if (lower_name == "basic_market_making" || lower_name == "basicmarketmaking" ||
        lower_name == "basic_mm") {
        return StrategyType::BASIC_MARKET_MAKING;
    }
    if (lower_name == "adaptive_market_making" || lower_name == "adaptivemarketmaking" ||
        lower_name == "adaptive_mm") {
        return StrategyType::ADAPTIVE_MARKET_MAKING;
    }
    if (lower_name == "inventory_market_making" || lower_name == "inventorymarketmaking" ||
        lower_name == "inventory_mm") {
        return StrategyType::INVENTORY_MARKET_MAKING;
    }
    if (lower_name == "multi_exchange_market_making" || lower_name == "multiexchangemarketmaking" ||
        lower_name == "multi_exchange_mm" || lower_name == "cross_exchange_mm") {
        return StrategyType::MULTI_EXCHANGE_MARKET_MAKING;
    }
    if (lower_name == "volatility_market_making" || lower_name == "volatilitymarketmaking" ||
        lower_name == "volatility_mm" || lower_name == "vol_mm") {
        return StrategyType::VOLATILITY_MARKET_MAKING;
    }

    return StrategyType::UNKNOWN;
}

// ============================================================================
// StrategyConfig
// ============================================================================

double StrategyConfig::get_param(const std::string& name, double default_value) const {
    auto it = base_params.numeric_params.find(name);
    return it != base_params.numeric_params.end() ? it->second : default_value;
}

std::string StrategyConfig::get_param(const std::string& name, const std::string& default_value) const {
    auto it = base_params.string_params.find(name);
    return it != base_params.string_params.end() ? it->second : default_value;
}

bool StrategyConfig::get_param(const std::string& name, bool default_value) const {
    auto it = base_params.bool_params.find(name);
    return it != base_params.bool_params.end() ? it->second : default_value;
}

// ============================================================================
// StrategyConfigManager
// ============================================================================

StrategyConfigManager& StrategyConfigManager::instance() {
    static StrategyConfigManager instance;
    return instance;
}

void StrategyConfigManager::load(const std::string& filepath) {
    config_filepath_ = filepath;

    YAML::Node root = YAML::LoadFile(filepath);

    std::unique_lock lock(mutex_);
    configs_.clear();

    // Parse strategies section
    if (root["strategies"]) {
        load_from_node(root["strategies"]);
    } else {
        load_from_node(root);
    }
}

void StrategyConfigManager::load_from_node(const YAML::Node& node) {
    if (!node.IsMap()) {
        return;
    }

    for (const auto& kv : node) {
        std::string id = kv.first.as<std::string>();
        StrategyConfig config = parse_strategy_config(id, kv.second);
        configs_[id] = std::move(config);
    }
}

StrategyConfig StrategyConfigManager::parse_strategy_config(
    const std::string& id, const YAML::Node& node) {

    StrategyConfig config;
    config.id = id;
    config.base_params = parse_base_params(node);

    // Determine strategy type
    std::string type_str = node["type"].as<std::string>("unknown");
    config.type = string_to_strategy_type(type_str);
    config.base_params.type = config.type;
    config.base_params.name = id;

    // Parse risk limits
    if (node["risk_limits"]) {
        config.base_params.risk_limits = parse_risk_limits(node["risk_limits"]);
    }

    // Parse trading pairs
    if (node["trading_pairs"]) {
        for (const auto& pair_node : node["trading_pairs"]) {
            config.base_params.trading_pairs.push_back(parse_pair_config(pair_node));
        }
    }

    // Parse type-specific parameters
    if (node["params"]) {
        switch (config.type) {
            case StrategyType::LATENCY_ARBITRAGE:
                config.latency_arb_params = parse_latency_arb_params(node["params"]);
                break;
            case StrategyType::TRIANGULAR_ARBITRAGE:
                config.triangular_arb_params = parse_triangular_arb_params(node["params"]);
                break;
            case StrategyType::STATISTICAL_ARBITRAGE:
                config.stat_arb_params = parse_stat_arb_params(node["params"]);
                break;
            case StrategyType::ORDER_FLOW_IMBALANCE:
                config.order_flow_params = parse_order_flow_params(node["params"]);
                break;
            case StrategyType::MICROSTRUCTURE_ALPHA:
                config.microstructure_params = parse_microstructure_params(node["params"]);
                break;
            case StrategyType::BASIC_MARKET_MAKING:
                config.basic_mm_params = parse_basic_mm_params(node["params"]);
                break;
            case StrategyType::ADAPTIVE_MARKET_MAKING:
                config.adaptive_mm_params = parse_adaptive_mm_params(node["params"]);
                break;
            case StrategyType::INVENTORY_MARKET_MAKING:
                config.inventory_mm_params = parse_inventory_mm_params(node["params"]);
                break;
            case StrategyType::MULTI_EXCHANGE_MARKET_MAKING:
                config.multi_exchange_mm_params = parse_multi_exchange_mm_params(node["params"]);
                break;
            case StrategyType::VOLATILITY_MARKET_MAKING:
                config.vol_mm_params = parse_vol_mm_params(node["params"]);
                break;
            default:
                break;
        }

        // Also store generic parameters
        for (const auto& kv : node["params"]) {
            std::string key = kv.first.as<std::string>();
            if (kv.second.IsScalar()) {
                // Try to determine type
                try {
                    config.base_params.numeric_params[key] = kv.second.as<double>();
                } catch (...) {
                    try {
                        config.base_params.bool_params[key] = kv.second.as<bool>();
                    } catch (...) {
                        config.base_params.string_params[key] = kv.second.as<std::string>();
                    }
                }
            }
        }
    }

    return config;
}

BaseStrategyParams StrategyConfigManager::parse_base_params(const YAML::Node& node) {
    BaseStrategyParams params;

    params.enabled = node["enabled"].as<bool>(false);
    params.priority = node["priority"].as<int>(0);
    params.capital_allocation = node["capital_allocation"].as<double>(0.0);
    params.capital_allocation_pct = node["capital_allocation_pct"].as<double>(0.0);
    params.tick_interval_us = node["tick_interval_us"].as<uint32_t>(1000);
    params.signal_timeout_ms = node["signal_timeout_ms"].as<uint32_t>(100);

    return params;
}

StrategyRiskLimits StrategyConfigManager::parse_risk_limits(const YAML::Node& node) {
    StrategyRiskLimits limits;

    // Position limits
    limits.max_position_size = node["max_position_size"].as<double>(0.0);
    limits.max_position_value = node["max_position_value"].as<double>(0.0);
    limits.max_position_pct = node["max_position_pct"].as<double>(1.0);

    // Order limits
    limits.max_order_size = node["max_order_size"].as<double>(0.0);
    limits.max_order_value = node["max_order_value"].as<double>(0.0);
    limits.max_open_orders = node["max_open_orders"].as<uint32_t>(100);
    limits.max_orders_per_second = node["max_orders_per_second"].as<uint32_t>(10);

    // Loss limits
    limits.max_daily_loss = node["max_daily_loss"].as<double>(0.0);
    limits.max_drawdown = node["max_drawdown"].as<double>(0.0);
    limits.max_loss_per_trade = node["max_loss_per_trade"].as<double>(0.0);

    // Profit limits
    limits.daily_profit_target = node["daily_profit_target"].as<double>(0.0);
    limits.stop_on_profit_target = node["stop_on_profit_target"].as<bool>(false);

    // Exposure limits
    limits.max_notional_exposure = node["max_notional_exposure"].as<double>(0.0);
    limits.max_delta_exposure = node["max_delta_exposure"].as<double>(0.0);
    limits.max_concentration = node["max_concentration"].as<double>(0.5);

    // Time-based limits
    limits.max_trades_per_minute = node["max_trades_per_minute"].as<uint32_t>(60);
    limits.max_trades_per_hour = node["max_trades_per_hour"].as<uint32_t>(1000);
    limits.cooldown_after_loss_ms = node["cooldown_after_loss_ms"].as<uint32_t>(0);

    return limits;
}

StrategyPairConfig StrategyConfigManager::parse_pair_config(const YAML::Node& node) {
    StrategyPairConfig pair;

    if (node.IsScalar()) {
        pair.symbol = node.as<std::string>();
        return pair;
    }

    pair.symbol = node["symbol"].as<std::string>("");
    pair.exchange = node["exchange"].as<std::string>("");
    pair.enabled = node["enabled"].as<bool>(true);
    pair.allocation_weight = node["allocation_weight"].as<double>(1.0);
    pair.min_spread_bps = node["min_spread_bps"].as<double>(0.0);
    pair.max_spread_bps = node["max_spread_bps"].as<double>(1000.0);

    if (node["max_position_size"]) {
        pair.max_position_size = node["max_position_size"].as<double>();
    }
    if (node["max_order_size"]) {
        pair.max_order_size = node["max_order_size"].as<double>();
    }

    return pair;
}

// ============================================================================
// Type-Specific Parameter Parsers
// ============================================================================

LatencyArbitrageParams StrategyConfigManager::parse_latency_arb_params(const YAML::Node& node) {
    LatencyArbitrageParams params;

    params.min_profit_bps = node["min_profit_bps"].as<double>(1.0);
    params.max_latency_ms = node["max_latency_ms"].as<double>(10.0);
    params.stale_price_threshold_ms = node["stale_price_threshold_ms"].as<double>(50.0);
    params.use_maker_orders = node["use_maker_orders"].as<bool>(false);
    params.aggression_factor = node["aggression_factor"].as<double>(1.0);
    params.confidence_threshold = node["confidence_threshold"].as<double>(0.8);

    if (node["fast_exchanges"]) {
        for (const auto& ex : node["fast_exchanges"]) {
            params.fast_exchanges.push_back(ex.as<std::string>());
        }
    }
    if (node["slow_exchanges"]) {
        for (const auto& ex : node["slow_exchanges"]) {
            params.slow_exchanges.push_back(ex.as<std::string>());
        }
    }

    return params;
}

TriangularArbitrageParams StrategyConfigManager::parse_triangular_arb_params(const YAML::Node& node) {
    TriangularArbitrageParams params;

    params.min_profit_bps = node["min_profit_bps"].as<double>(1.0);
    params.max_leg_slippage_bps = node["max_leg_slippage_bps"].as<double>(5.0);
    params.max_execution_time_ms = node["max_execution_time_ms"].as<uint32_t>(100);
    params.partial_fills_allowed = node["partial_fills_allowed"].as<bool>(false);
    params.volume_threshold = node["volume_threshold"].as<double>(1000.0);

    if (node["triangles"]) {
        for (const auto& triangle : node["triangles"]) {
            std::vector<std::string> legs;
            for (const auto& leg : triangle) {
                legs.push_back(leg.as<std::string>());
            }
            params.triangles.push_back(legs);
        }
    }

    return params;
}

StatisticalArbitrageParams StrategyConfigManager::parse_stat_arb_params(const YAML::Node& node) {
    StatisticalArbitrageParams params;

    params.entry_z_score = node["entry_z_score"].as<double>(2.0);
    params.exit_z_score = node["exit_z_score"].as<double>(0.5);
    params.stop_loss_z_score = node["stop_loss_z_score"].as<double>(4.0);
    params.lookback_period = node["lookback_period"].as<uint32_t>(100);
    params.half_life = node["half_life"].as<double>(20.0);
    params.correlation_threshold = node["correlation_threshold"].as<double>(0.7);
    params.cointegration_pvalue = node["cointegration_pvalue"].as<double>(0.05);
    params.dynamic_hedge_ratio = node["dynamic_hedge_ratio"].as<bool>(true);
    params.rebalance_interval_ms = node["rebalance_interval_ms"].as<uint32_t>(60000);

    if (node["pairs"]) {
        for (const auto& pair : node["pairs"]) {
            if (pair.IsSequence() && pair.size() >= 2) {
                params.pairs.emplace_back(
                    pair[0].as<std::string>(),
                    pair[1].as<std::string>()
                );
            }
        }
    }

    return params;
}

OrderFlowImbalanceParams StrategyConfigManager::parse_order_flow_params(const YAML::Node& node) {
    OrderFlowImbalanceParams params;

    params.imbalance_threshold = node["imbalance_threshold"].as<double>(0.6);
    params.depth_levels = node["depth_levels"].as<uint32_t>(10);
    params.flow_window_ms = node["flow_window_ms"].as<uint32_t>(1000);
    params.volume_threshold = node["volume_threshold"].as<double>(10000.0);
    params.momentum_factor = node["momentum_factor"].as<double>(0.5);
    params.use_trade_imbalance = node["use_trade_imbalance"].as<bool>(true);
    params.use_cancel_imbalance = node["use_cancel_imbalance"].as<bool>(true);
    params.aggression_decay = node["aggression_decay"].as<double>(0.95);

    return params;
}

MicrostructureAlphaParams StrategyConfigManager::parse_microstructure_params(const YAML::Node& node) {
    MicrostructureAlphaParams params;

    params.tick_intensity_threshold = node["tick_intensity_threshold"].as<double>(0.7);
    params.spread_percentile = node["spread_percentile"].as<double>(0.25);
    params.book_pressure_depth = node["book_pressure_depth"].as<uint32_t>(5);
    params.queue_position_alpha = node["queue_position_alpha"].as<double>(0.3);
    params.use_tick_rule = node["use_tick_rule"].as<bool>(true);
    params.microprice_weight = node["microprice_weight"].as<double>(0.5);
    params.signal_decay_ticks = node["signal_decay_ticks"].as<uint32_t>(10);

    return params;
}

BasicMarketMakingParams StrategyConfigManager::parse_basic_mm_params(const YAML::Node& node) {
    BasicMarketMakingParams params;

    params.spread_bps = node["spread_bps"].as<double>(10.0);
    params.skew_factor = node["skew_factor"].as<double>(0.5);
    params.num_levels = node["num_levels"].as<uint32_t>(3);
    params.level_spacing_bps = node["level_spacing_bps"].as<double>(2.0);
    params.quote_size = node["quote_size"].as<double>(100.0);
    params.min_spread_bps = node["min_spread_bps"].as<double>(5.0);
    params.max_spread_bps = node["max_spread_bps"].as<double>(50.0);
    params.fade_inventory = node["fade_inventory"].as<bool>(true);
    params.inventory_skew_factor = node["inventory_skew_factor"].as<double>(0.1);
    params.quote_refresh_ms = node["quote_refresh_ms"].as<uint32_t>(100);

    return params;
}

AdaptiveMarketMakingParams StrategyConfigManager::parse_adaptive_mm_params(const YAML::Node& node) {
    AdaptiveMarketMakingParams params;

    params.base_spread_bps = node["base_spread_bps"].as<double>(10.0);
    params.volatility_scaling = node["volatility_scaling"].as<double>(1.0);
    params.inventory_aversion = node["inventory_aversion"].as<double>(0.5);
    params.adverse_selection_factor = node["adverse_selection_factor"].as<double>(0.3);
    params.regime_lookback = node["regime_lookback"].as<uint32_t>(100);
    params.use_fair_value_model = node["use_fair_value_model"].as<bool>(true);
    params.learning_rate = node["learning_rate"].as<double>(0.01);
    params.volatility_model = node["volatility_model"].as<std::string>("ewma");
    params.vol_ewma_span = node["vol_ewma_span"].as<double>(100.0);

    return params;
}

InventoryMarketMakingParams StrategyConfigManager::parse_inventory_mm_params(const YAML::Node& node) {
    InventoryMarketMakingParams params;

    params.target_inventory = node["target_inventory"].as<double>(0.0);
    params.inventory_half_life = node["inventory_half_life"].as<double>(300.0);
    params.risk_aversion = node["risk_aversion"].as<double>(0.5);
    params.max_inventory = node["max_inventory"].as<double>(1000.0);
    params.skew_intensity = node["skew_intensity"].as<double>(1.0);
    params.use_avellaneda_stoikov = node["use_avellaneda_stoikov"].as<bool>(true);
    params.terminal_time = node["terminal_time"].as<double>(3600.0);
    params.gamma = node["gamma"].as<double>(0.1);

    return params;
}

MultiExchangeMMParams StrategyConfigManager::parse_multi_exchange_mm_params(const YAML::Node& node) {
    MultiExchangeMMParams params;

    if (node["exchanges"]) {
        for (const auto& ex : node["exchanges"]) {
            params.exchanges.push_back(ex.as<std::string>());
        }
    }

    params.min_cross_exchange_spread = node["min_cross_exchange_spread"].as<double>(5.0);
    params.latency_penalty_factor = node["latency_penalty_factor"].as<double>(0.1);
    params.enable_hedging = node["enable_hedging"].as<bool>(true);
    params.hedge_ratio = node["hedge_ratio"].as<double>(1.0);
    params.max_hedge_delay_ms = node["max_hedge_delay_ms"].as<uint32_t>(50);
    params.concentration_limit = node["concentration_limit"].as<double>(0.5);

    return params;
}

VolatilityMarketMakingParams StrategyConfigManager::parse_vol_mm_params(const YAML::Node& node) {
    VolatilityMarketMakingParams params;

    params.vol_regime_threshold = node["vol_regime_threshold"].as<double>(0.02);
    params.spread_vol_multiplier = node["spread_vol_multiplier"].as<double>(5.0);
    params.gamma_scalping_threshold = node["gamma_scalping_threshold"].as<double>(0.01);
    params.adjust_for_skew = node["adjust_for_skew"].as<bool>(true);
    params.delta_hedge_threshold = node["delta_hedge_threshold"].as<double>(0.1);
    params.vol_update_interval_ms = node["vol_update_interval_ms"].as<uint32_t>(1000);
    params.implied_vol_model = node["implied_vol_model"].as<std::string>("sabr");

    return params;
}

// ============================================================================
// Access Methods
// ============================================================================

const StrategyConfig* StrategyConfigManager::get(const std::string& strategy_id) const {
    std::shared_lock lock(mutex_);
    auto it = configs_.find(strategy_id);
    return it != configs_.end() ? &it->second : nullptr;
}

std::vector<const StrategyConfig*> StrategyConfigManager::get_enabled_strategies() const {
    std::shared_lock lock(mutex_);
    std::vector<const StrategyConfig*> enabled;

    for (const auto& [id, config] : configs_) {
        if (config.is_enabled()) {
            enabled.push_back(&config);
        }
    }

    // Sort by priority
    std::sort(enabled.begin(), enabled.end(),
        [](const StrategyConfig* a, const StrategyConfig* b) {
            return a->base_params.priority > b->base_params.priority;
        });

    return enabled;
}

std::vector<const StrategyConfig*> StrategyConfigManager::get_strategies_by_type(StrategyType type) const {
    std::shared_lock lock(mutex_);
    std::vector<const StrategyConfig*> result;

    for (const auto& [id, config] : configs_) {
        if (config.type == type) {
            result.push_back(&config);
        }
    }

    return result;
}

std::vector<std::string> StrategyConfigManager::get_all_strategy_ids() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(configs_.size());

    for (const auto& [id, config] : configs_) {
        ids.push_back(id);
    }

    return ids;
}

bool StrategyConfigManager::is_enabled(const std::string& strategy_id) const {
    auto config = get(strategy_id);
    return config && config->is_enabled();
}

void StrategyConfigManager::update_params(const std::string& strategy_id, const YAML::Node& params) {
    std::unique_lock lock(mutex_);

    auto it = configs_.find(strategy_id);
    if (it == configs_.end()) {
        return;
    }

    // Update generic parameters
    for (const auto& kv : params) {
        std::string key = kv.first.as<std::string>();
        if (kv.second.IsScalar()) {
            try {
                it->second.base_params.numeric_params[key] = kv.second.as<double>();
            } catch (...) {
                try {
                    it->second.base_params.bool_params[key] = kv.second.as<bool>();
                } catch (...) {
                    it->second.base_params.string_params[key] = kv.second.as<std::string>();
                }
            }
        }
    }
}

void StrategyConfigManager::reload() {
    if (!config_filepath_.empty()) {
        load(config_filepath_);
    }
}

void StrategyConfigManager::clear() {
    std::unique_lock lock(mutex_);
    configs_.clear();
}

} // namespace config
} // namespace hft
