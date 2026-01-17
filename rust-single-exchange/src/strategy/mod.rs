//! Market making strategies

use crate::core::types::*;
use crate::orderbook::OrderBook;

mod market_maker;

pub use market_maker::{BasicMarketMaker, AvellanedaStoikovMM};

/// Signal data for strategy decisions
#[derive(Debug, Clone, Copy, Default)]
pub struct Signal {
    pub fair_value: f64,
    pub volatility: f64,
    pub momentum: f64,
    pub inventory_pressure: f64,
    pub timestamp: Timestamp,
}

/// Quote decision from strategy
#[derive(Debug, Clone, Default)]
pub struct QuoteDecision {
    pub should_quote: bool,
    pub bid_price: Price,
    pub ask_price: Price,
    pub bid_size: Quantity,
    pub ask_size: Quantity,
    pub reason: String,
}

/// Market making strategy parameters
#[derive(Debug, Clone)]
pub struct MarketMakerParams {
    pub min_spread_bps: f64,
    pub max_spread_bps: f64,
    pub target_spread_bps: f64,
    pub max_position: Quantity,
    pub inventory_skew: f64,
    pub default_order_size: Quantity,
    pub min_order_size: Quantity,
    pub max_order_size: Quantity,
    pub quote_refresh_us: u64,
    pub min_quote_life_us: u64,
}

impl Default for MarketMakerParams {
    fn default() -> Self {
        MarketMakerParams {
            min_spread_bps: 5.0,
            max_spread_bps: 50.0,
            target_spread_bps: 10.0,
            max_position: to_qty(1.0),
            inventory_skew: 0.5,
            default_order_size: to_qty(0.001),
            min_order_size: to_qty(0.0001),
            max_order_size: to_qty(0.1),
            quote_refresh_us: 100_000,
            min_quote_life_us: 50_000,
        }
    }
}

/// Market maker strategy trait
pub trait MarketMaker {
    /// Compute quotes based on current market state
    fn compute_quotes(
        &mut self,
        book: &OrderBook,
        position: Quantity,
        signal: &Signal,
    ) -> QuoteDecision;

    /// Handle trade fill
    fn on_fill(&mut self, order: &Order, filled_qty: Quantity, fill_price: Price);

    /// Handle order cancel
    fn on_cancel(&mut self, order_id: OrderId);

    /// Get strategy parameters
    fn params(&self) -> &MarketMakerParams;

    /// Update strategy parameters
    fn update_params(&mut self, params: MarketMakerParams);

    /// Enable/disable strategy
    fn set_enabled(&mut self, enabled: bool);

    /// Check if strategy is enabled
    fn is_enabled(&self) -> bool;
}
