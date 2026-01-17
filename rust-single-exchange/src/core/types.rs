//! Core types for the HFT system
//!
//! All prices and quantities are represented as fixed-point integers
//! to avoid floating-point precision issues.

use rust_decimal::Decimal;
use serde::{Deserialize, Serialize};
use std::fmt;
use std::time::{SystemTime, UNIX_EPOCH};

/// Price represented as integer (8 decimal places)
pub type Price = i64;

/// Quantity represented as integer (8 decimal places)
pub type Quantity = i64;

/// Order identifier
pub type OrderId = u64;

/// Timestamp in nanoseconds since epoch
pub type Timestamp = u64;

/// Sequence number for ordering events
pub type SequenceNum = u64;

/// Precision for price/quantity conversions
pub const PRECISION: i64 = 100_000_000;

// ============================================================================
// Enums
// ============================================================================

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[repr(u8)]
pub enum Side {
    Buy = 0,
    Sell = 1,
}

impl Side {
    pub fn opposite(&self) -> Self {
        match self {
            Side::Buy => Side::Sell,
            Side::Sell => Side::Buy,
        }
    }
}

impl fmt::Display for Side {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Side::Buy => write!(f, "BUY"),
            Side::Sell => write!(f, "SELL"),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[repr(u8)]
pub enum OrderType {
    Limit = 0,
    Market = 1,
    LimitMaker = 2,
    Ioc = 3,
    Fok = 4,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[repr(u8)]
pub enum OrderStatus {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Canceled = 3,
    Rejected = 4,
    Expired = 5,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[repr(u8)]
pub enum TimeInForce {
    Gtc = 0,  // Good till cancel
    Ioc = 1,  // Immediate or cancel
    Fok = 2,  // Fill or kill
    Gtx = 3,  // Good till crossing (post-only)
}

// ============================================================================
// Symbol
// ============================================================================

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Symbol(pub String);

impl Symbol {
    pub fn new(s: impl Into<String>) -> Self {
        Symbol(s.into())
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for Symbol {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl From<&str> for Symbol {
    fn from(s: &str) -> Self {
        Symbol(s.to_string())
    }
}

impl From<String> for Symbol {
    fn from(s: String) -> Self {
        Symbol(s)
    }
}

// ============================================================================
// Order
// ============================================================================

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Order {
    pub id: OrderId,
    pub client_id: OrderId,
    pub symbol: Symbol,
    pub side: Side,
    pub order_type: OrderType,
    pub time_in_force: TimeInForce,
    pub price: Price,
    pub quantity: Quantity,
    pub filled_qty: Quantity,
    pub status: OrderStatus,
    pub timestamp: Timestamp,
}

impl Order {
    pub fn new(
        symbol: Symbol,
        side: Side,
        order_type: OrderType,
        price: Price,
        quantity: Quantity,
    ) -> Self {
        Order {
            id: 0,
            client_id: 0,
            symbol,
            side,
            order_type,
            time_in_force: TimeInForce::Gtc,
            price,
            quantity,
            filled_qty: 0,
            status: OrderStatus::New,
            timestamp: now_nanos(),
        }
    }

    pub fn remaining(&self) -> Quantity {
        self.quantity - self.filled_qty
    }

    pub fn is_active(&self) -> bool {
        matches!(
            self.status,
            OrderStatus::New | OrderStatus::PartiallyFilled
        )
    }
}

// ============================================================================
// Quote
// ============================================================================

#[derive(Debug, Clone, Copy, Default)]
pub struct Quote {
    pub bid_price: Price,
    pub ask_price: Price,
    pub bid_qty: Quantity,
    pub ask_qty: Quantity,
    pub timestamp: Timestamp,
}

impl Quote {
    pub fn spread(&self) -> Price {
        self.ask_price - self.bid_price
    }

    pub fn mid_price(&self) -> Price {
        (self.bid_price + self.ask_price) / 2
    }

    pub fn spread_bps(&self) -> f64 {
        let mid = self.mid_price();
        if mid == 0 {
            return 0.0;
        }
        10000.0 * (self.spread() as f64) / (mid as f64)
    }
}

// ============================================================================
// Trade
// ============================================================================

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Trade {
    pub order_id: OrderId,
    pub trade_id: OrderId,
    pub symbol: Symbol,
    pub side: Side,
    pub price: Price,
    pub quantity: Quantity,
    pub timestamp: Timestamp,
    pub is_maker: bool,
}

// ============================================================================
// Market Data Tick
// ============================================================================

#[derive(Debug, Clone, Copy, Default)]
pub struct Tick {
    pub bid: Price,
    pub ask: Price,
    pub bid_qty: Quantity,
    pub ask_qty: Quantity,
    pub last_price: Price,
    pub last_qty: Quantity,
    pub exchange_ts: Timestamp,
    pub local_ts: Timestamp,
    pub sequence: SequenceNum,
}

// ============================================================================
// Conversion Functions
// ============================================================================

/// Convert decimal to fixed-point price
pub fn to_price(value: f64) -> Price {
    (value * PRECISION as f64) as Price
}

/// Convert fixed-point price to decimal
pub fn from_price(price: Price) -> f64 {
    price as f64 / PRECISION as f64
}

/// Convert decimal to fixed-point quantity
pub fn to_qty(value: f64) -> Quantity {
    (value * PRECISION as f64) as Quantity
}

/// Convert fixed-point quantity to decimal
pub fn from_qty(qty: Quantity) -> f64 {
    qty as f64 / PRECISION as f64
}

/// Convert Decimal to Price
pub fn decimal_to_price(d: Decimal) -> Price {
    (d * Decimal::from(PRECISION)).to_string().parse().unwrap_or(0)
}

/// Convert Price to Decimal
pub fn price_to_decimal(p: Price) -> Decimal {
    Decimal::from(p) / Decimal::from(PRECISION)
}

/// Get current timestamp in nanoseconds
pub fn now_nanos() -> Timestamp {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as Timestamp)
        .unwrap_or(0)
}

/// Get current timestamp in microseconds
pub fn now_micros() -> Timestamp {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_micros() as Timestamp)
        .unwrap_or(0)
}

/// Get current timestamp in milliseconds
pub fn now_millis() -> Timestamp {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as Timestamp)
        .unwrap_or(0)
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_price_conversion() {
        let price = 50000.12345678;
        let fixed = to_price(price);
        let back = from_price(fixed);
        assert!((price - back).abs() < 1e-8);
    }

    #[test]
    fn test_side_opposite() {
        assert_eq!(Side::Buy.opposite(), Side::Sell);
        assert_eq!(Side::Sell.opposite(), Side::Buy);
    }

    #[test]
    fn test_order_remaining() {
        let mut order = Order::new(
            Symbol::new("BTCUSDT"),
            Side::Buy,
            OrderType::Limit,
            to_price(50000.0),
            to_qty(1.0),
        );
        assert_eq!(order.remaining(), to_qty(1.0));

        order.filled_qty = to_qty(0.5);
        assert_eq!(order.remaining(), to_qty(0.5));
    }
}
