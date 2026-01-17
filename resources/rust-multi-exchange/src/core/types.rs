//! Core types for multi-exchange HFT system

use serde::{Deserialize, Serialize};
use std::fmt;

pub type Price = i64;
pub type Quantity = i64;
pub type OrderId = u64;
pub type Timestamp = u64;
pub const PRECISION: i64 = 100_000_000;

/// Exchange identifier
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[repr(u8)]
pub enum ExchangeId {
    Unknown = 0,
    Binance = 1,
    Bybit = 2,
    Okx = 3,
    Coinbase = 4,
    Kraken = 5,
}

impl fmt::Display for ExchangeId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ExchangeId::Binance => write!(f, "BINANCE"),
            ExchangeId::Bybit => write!(f, "BYBIT"),
            ExchangeId::Okx => write!(f, "OKX"),
            ExchangeId::Coinbase => write!(f, "COINBASE"),
            ExchangeId::Kraken => write!(f, "KRAKEN"),
            ExchangeId::Unknown => write!(f, "UNKNOWN"),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum Side { Buy, Sell }

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum OrderStatus { New, PartiallyFilled, Filled, Canceled, Rejected }

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Symbol(pub String);

impl Symbol {
    pub fn new(s: impl Into<String>) -> Self { Symbol(s.into()) }
    pub fn as_str(&self) -> &str { &self.0 }
}

impl fmt::Display for Symbol {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

/// Order with exchange attribution
#[derive(Debug, Clone)]
pub struct Order {
    pub id: OrderId,
    pub exchange: ExchangeId,
    pub symbol: Symbol,
    pub side: Side,
    pub price: Price,
    pub quantity: Quantity,
    pub filled_qty: Quantity,
    pub status: OrderStatus,
    pub timestamp: Timestamp,
}

/// Quote from a specific exchange
#[derive(Debug, Clone, Copy, Default)]
pub struct ExchangeQuote {
    pub exchange: ExchangeId,
    pub bid: Price,
    pub ask: Price,
    pub bid_qty: Quantity,
    pub ask_qty: Quantity,
    pub timestamp: Timestamp,
    pub latency_ns: Timestamp,
}

/// Arbitrage opportunity between exchanges
#[derive(Debug, Clone)]
pub struct ArbitrageOpportunity {
    pub symbol: Symbol,
    pub buy_exchange: ExchangeId,
    pub sell_exchange: ExchangeId,
    pub buy_price: Price,
    pub sell_price: Price,
    pub quantity: Quantity,
    pub profit_bps: f64,
    pub timestamp: Timestamp,
}

// Conversion functions
pub fn to_price(v: f64) -> Price { (v * PRECISION as f64) as Price }
pub fn from_price(p: Price) -> f64 { p as f64 / PRECISION as f64 }
pub fn to_qty(v: f64) -> Quantity { (v * PRECISION as f64) as Quantity }
pub fn from_qty(q: Quantity) -> f64 { q as f64 / PRECISION as f64 }

pub fn now_nanos() -> Timestamp {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos() as Timestamp)
        .unwrap_or(0)
}
