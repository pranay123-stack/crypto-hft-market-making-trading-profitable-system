//! # Multi-Exchange Crypto HFT Library
//!
//! High-performance market making system supporting multiple exchanges
//! with cross-exchange arbitrage capabilities.

pub mod core;
pub mod exchange;
pub mod orderbook;
pub mod strategy;
pub mod risk;
pub mod arbitrage;

pub mod prelude {
    pub use crate::core::types::*;
    pub use crate::exchange::{ExchangeId, ExchangeManager};
    pub use crate::orderbook::{ConsolidatedBook, NBBO};
    pub use crate::strategy::CrossExchangeMM;
    pub use crate::arbitrage::{ArbitrageDetector, ArbitrageOpportunity};
    pub use crate::risk::RiskManager;
}
