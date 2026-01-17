//! Configuration management

use crate::core::types::*;
use crate::exchange::ExchangeConfig;
use crate::risk::RiskLimits;
use crate::strategy::MarketMakerParams;
use serde::{Deserialize, Serialize};
use std::path::Path;

/// Full application configuration
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppConfig {
    pub trading: TradingConfig,
    pub exchange: ExchangeConfigFile,
    pub strategy: StrategyConfig,
    pub risk: RiskConfig,
    pub system: SystemConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TradingConfig {
    pub symbol: String,
    pub base_asset: String,
    pub quote_asset: String,
    pub paper_trading: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ExchangeConfigFile {
    pub name: String,
    pub rest_url: String,
    pub ws_url: String,
    pub api_key: String,
    pub api_secret: String,
    #[serde(default)]
    pub passphrase: Option<String>,
    #[serde(default = "default_timeout")]
    pub connect_timeout_ms: u64,
    #[serde(default = "default_rate_limit")]
    pub max_requests_per_second: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StrategyConfig {
    pub min_spread_bps: f64,
    pub max_spread_bps: f64,
    pub target_spread_bps: f64,
    pub max_position: f64,
    pub inventory_skew: f64,
    pub default_order_size: f64,
    pub min_order_size: f64,
    pub max_order_size: f64,
    #[serde(default = "default_quote_refresh")]
    pub quote_refresh_us: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RiskConfig {
    pub max_position_qty: f64,
    pub max_position_value: f64,
    pub max_order_qty: f64,
    pub max_order_value: f64,
    pub max_orders_per_second: u32,
    pub max_open_orders: u32,
    pub max_daily_loss: f64,
    pub max_drawdown: f64,
    #[serde(default = "default_true")]
    pub kill_switch_enabled: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemConfig {
    #[serde(default = "default_log_level")]
    pub log_level: String,
    #[serde(default = "default_log_dir")]
    pub log_dir: String,
    #[serde(default = "default_true")]
    pub log_to_console: bool,
    #[serde(default)]
    pub log_to_file: bool,
    #[serde(default = "default_buffer_size")]
    pub tick_buffer_size: usize,
}

// Default value functions
fn default_timeout() -> u64 { 5000 }
fn default_rate_limit() -> u32 { 10 }
fn default_quote_refresh() -> u64 { 100_000 }
fn default_true() -> bool { true }
fn default_log_level() -> String { "INFO".to_string() }
fn default_log_dir() -> String { "./logs".to_string() }
fn default_buffer_size() -> usize { 65536 }

impl Default for AppConfig {
    fn default() -> Self {
        AppConfig {
            trading: TradingConfig {
                symbol: "BTCUSDT".to_string(),
                base_asset: "BTC".to_string(),
                quote_asset: "USDT".to_string(),
                paper_trading: true,
            },
            exchange: ExchangeConfigFile {
                name: "binance".to_string(),
                rest_url: "https://testnet.binance.vision".to_string(),
                ws_url: "wss://testnet.binance.vision/ws".to_string(),
                api_key: String::new(),
                api_secret: String::new(),
                passphrase: None,
                connect_timeout_ms: 5000,
                max_requests_per_second: 10,
            },
            strategy: StrategyConfig {
                min_spread_bps: 5.0,
                max_spread_bps: 50.0,
                target_spread_bps: 10.0,
                max_position: 0.1,
                inventory_skew: 0.5,
                default_order_size: 0.001,
                min_order_size: 0.0001,
                max_order_size: 0.01,
                quote_refresh_us: 100_000,
            },
            risk: RiskConfig {
                max_position_qty: 0.1,
                max_position_value: 10000.0,
                max_order_qty: 0.01,
                max_order_value: 1000.0,
                max_orders_per_second: 10,
                max_open_orders: 20,
                max_daily_loss: 100.0,
                max_drawdown: 200.0,
                kill_switch_enabled: true,
            },
            system: SystemConfig {
                log_level: "INFO".to_string(),
                log_dir: "./logs".to_string(),
                log_to_console: true,
                log_to_file: false,
                tick_buffer_size: 65536,
            },
        }
    }
}

impl AppConfig {
    /// Load configuration from file
    pub fn load<P: AsRef<Path>>(path: P) -> anyhow::Result<Self> {
        let content = std::fs::read_to_string(path)?;
        let config: AppConfig = serde_json::from_str(&content)?;
        Ok(config)
    }

    /// Save configuration to file
    pub fn save<P: AsRef<Path>>(&self, path: P) -> anyhow::Result<()> {
        let content = serde_json::to_string_pretty(self)?;
        std::fs::write(path, content)?;
        Ok(())
    }

    /// Apply environment variable overrides
    pub fn apply_env_overrides(&mut self) {
        if let Ok(key) = std::env::var("API_KEY") {
            self.exchange.api_key = key;
        }
        if let Ok(secret) = std::env::var("API_SECRET") {
            self.exchange.api_secret = secret;
        }
        if let Ok(symbol) = std::env::var("TRADING_SYMBOL") {
            self.trading.symbol = symbol;
        }
        if let Ok(level) = std::env::var("LOG_LEVEL") {
            self.system.log_level = level;
        }
    }

    /// Convert to exchange config
    pub fn to_exchange_config(&self) -> ExchangeConfig {
        ExchangeConfig {
            name: self.exchange.name.clone(),
            rest_url: self.exchange.rest_url.clone(),
            ws_url: self.exchange.ws_url.clone(),
            api_key: self.exchange.api_key.clone(),
            api_secret: self.exchange.api_secret.clone(),
            passphrase: self.exchange.passphrase.clone(),
            connect_timeout_ms: self.exchange.connect_timeout_ms,
            read_timeout_ms: 1000,
            max_requests_per_second: self.exchange.max_requests_per_second,
            testnet: self.trading.paper_trading,
        }
    }

    /// Convert to strategy params
    pub fn to_strategy_params(&self) -> MarketMakerParams {
        MarketMakerParams {
            min_spread_bps: self.strategy.min_spread_bps,
            max_spread_bps: self.strategy.max_spread_bps,
            target_spread_bps: self.strategy.target_spread_bps,
            max_position: to_qty(self.strategy.max_position),
            inventory_skew: self.strategy.inventory_skew,
            default_order_size: to_qty(self.strategy.default_order_size),
            min_order_size: to_qty(self.strategy.min_order_size),
            max_order_size: to_qty(self.strategy.max_order_size),
            quote_refresh_us: self.strategy.quote_refresh_us,
            min_quote_life_us: 50_000,
        }
    }

    /// Convert to risk limits
    pub fn to_risk_limits(&self) -> RiskLimits {
        RiskLimits {
            max_position_qty: to_qty(self.risk.max_position_qty),
            max_position_value: self.risk.max_position_value,
            max_order_qty: to_qty(self.risk.max_order_qty),
            max_order_value: self.risk.max_order_value,
            max_orders_per_second: self.risk.max_orders_per_second,
            max_open_orders: self.risk.max_open_orders,
            max_daily_loss: self.risk.max_daily_loss,
            max_drawdown: self.risk.max_drawdown,
            max_deviation_bps: 100.0,
            kill_switch_enabled: self.risk.kill_switch_enabled,
        }
    }
}
