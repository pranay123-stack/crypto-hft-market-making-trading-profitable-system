//! # Crypto HFT Market Making Library
//!
//! A high-performance market making system for cryptocurrency exchanges.
//!
//! ## Features
//! - Ultra-low latency order book management
//! - Multiple market making strategies
//! - Real-time risk management
//! - Binance exchange support (extensible)

pub mod core;
pub mod exchange;
pub mod orderbook;
pub mod strategy;
pub mod risk;
pub mod utils;

pub use core::types::*;
pub use core::engine::TradingEngine;
pub use orderbook::OrderBook;
pub use strategy::MarketMaker;
pub use risk::RiskManager;

/// Prelude module for convenient imports
pub mod prelude {
    pub use crate::core::types::*;
    pub use crate::core::engine::{TradingEngine, EngineConfig};
    pub use crate::orderbook::{OrderBook, PriceLevel};
    pub use crate::strategy::{MarketMaker, MarketMakerParams, QuoteDecision};
    pub use crate::risk::{RiskManager, RiskLimits, RiskCheckResult};
    pub use crate::exchange::{ExchangeClient, ExchangeConfig};
}
