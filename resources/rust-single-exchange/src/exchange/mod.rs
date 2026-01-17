//! Exchange client module

use crate::core::types::*;
use async_trait::async_trait;
use thiserror::Error;

pub mod binance;

pub use binance::BinanceClient;

/// Exchange errors
#[derive(Debug, Error)]
pub enum ExchangeError {
    #[error("Connection failed: {0}")]
    ConnectionFailed(String),

    #[error("Authentication failed: {0}")]
    AuthenticationFailed(String),

    #[error("Request failed: {0}")]
    RequestFailed(String),

    #[error("Rate limit exceeded")]
    RateLimitExceeded,

    #[error("Order rejected: {0}")]
    OrderRejected(String),

    #[error("Parse error: {0}")]
    ParseError(String),

    #[error("WebSocket error: {0}")]
    WebSocketError(String),

    #[error("Timeout")]
    Timeout,
}

/// Exchange configuration
#[derive(Debug, Clone)]
pub struct ExchangeConfig {
    pub name: String,
    pub rest_url: String,
    pub ws_url: String,
    pub api_key: String,
    pub api_secret: String,
    pub passphrase: Option<String>,

    pub connect_timeout_ms: u64,
    pub read_timeout_ms: u64,
    pub max_requests_per_second: u32,
    pub testnet: bool,
}

impl Default for ExchangeConfig {
    fn default() -> Self {
        ExchangeConfig {
            name: "binance".to_string(),
            rest_url: "https://api.binance.com".to_string(),
            ws_url: "wss://stream.binance.com:9443/ws".to_string(),
            api_key: String::new(),
            api_secret: String::new(),
            passphrase: None,
            connect_timeout_ms: 5000,
            read_timeout_ms: 1000,
            max_requests_per_second: 10,
            testnet: false,
        }
    }
}

/// Order request
#[derive(Debug, Clone)]
pub struct OrderRequest {
    pub symbol: Symbol,
    pub side: Side,
    pub order_type: OrderType,
    pub time_in_force: TimeInForce,
    pub price: Price,
    pub quantity: Quantity,
    pub client_order_id: Option<String>,
}

/// Order response
#[derive(Debug, Clone)]
pub struct OrderResponse {
    pub success: bool,
    pub order_id: OrderId,
    pub client_order_id: String,
    pub status: OrderStatus,
    pub error_message: Option<String>,
}

/// Cancel response
#[derive(Debug, Clone)]
pub struct CancelResponse {
    pub success: bool,
    pub order_id: OrderId,
    pub error_message: Option<String>,
}

/// Exchange callbacks
pub struct ExchangeCallbacks {
    pub on_tick: Option<Box<dyn Fn(Tick) + Send + Sync>>,
    pub on_order_update: Option<Box<dyn Fn(Order) + Send + Sync>>,
    pub on_trade: Option<Box<dyn Fn(Trade) + Send + Sync>>,
    pub on_error: Option<Box<dyn Fn(String) + Send + Sync>>,
    pub on_connected: Option<Box<dyn Fn() + Send + Sync>>,
    pub on_disconnected: Option<Box<dyn Fn() + Send + Sync>>,
}

impl Default for ExchangeCallbacks {
    fn default() -> Self {
        ExchangeCallbacks {
            on_tick: None,
            on_order_update: None,
            on_trade: None,
            on_error: None,
            on_connected: None,
            on_disconnected: None,
        }
    }
}

/// Exchange client trait
#[async_trait]
pub trait ExchangeClient: Send + Sync {
    /// Get exchange name
    fn name(&self) -> &str;

    /// Connect to exchange
    async fn connect(&mut self) -> Result<(), ExchangeError>;

    /// Disconnect from exchange
    async fn disconnect(&mut self) -> Result<(), ExchangeError>;

    /// Check if connected
    fn is_connected(&self) -> bool;

    /// Subscribe to ticker
    async fn subscribe_ticker(&mut self, symbol: &Symbol) -> Result<(), ExchangeError>;

    /// Subscribe to orderbook
    async fn subscribe_orderbook(&mut self, symbol: &Symbol, depth: u32) -> Result<(), ExchangeError>;

    /// Subscribe to trades
    async fn subscribe_trades(&mut self, symbol: &Symbol) -> Result<(), ExchangeError>;

    /// Send order
    async fn send_order(&self, request: OrderRequest) -> Result<OrderResponse, ExchangeError>;

    /// Cancel order
    async fn cancel_order(&self, symbol: &Symbol, order_id: OrderId) -> Result<CancelResponse, ExchangeError>;

    /// Cancel all orders
    async fn cancel_all_orders(&self, symbol: &Symbol) -> Result<(), ExchangeError>;

    /// Get balance
    async fn get_balance(&self, asset: &str) -> Result<f64, ExchangeError>;

    /// Get open orders
    async fn get_open_orders(&self, symbol: &Symbol) -> Result<Vec<Order>, ExchangeError>;

    /// Set callbacks
    fn set_callbacks(&mut self, callbacks: ExchangeCallbacks);

    /// Get server time
    async fn server_time(&self) -> Result<Timestamp, ExchangeError>;
}
