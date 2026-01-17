//! Arbitrage detection and execution

use crate::core::types::*;
use crate::orderbook::ConsolidatedBook;
use std::sync::atomic::{AtomicU64, Ordering};

pub use crate::core::types::ArbitrageOpportunity;

/// Configuration for arbitrage detection
#[derive(Debug, Clone)]
pub struct ArbitrageConfig {
    pub min_profit_bps: f64,
    pub max_slippage_bps: f64,
    pub min_quantity: Quantity,
    pub max_quantity: Quantity,
    pub max_age_ns: Timestamp,
}

impl Default for ArbitrageConfig {
    fn default() -> Self {
        ArbitrageConfig {
            min_profit_bps: 5.0,
            max_slippage_bps: 2.0,
            min_quantity: to_qty(0.001),
            max_quantity: to_qty(0.1),
            max_age_ns: 100_000_000, // 100ms
        }
    }
}

/// Arbitrage detector
pub struct ArbitrageDetector {
    config: ArbitrageConfig,
    opportunities_found: AtomicU64,
    opportunities_executed: AtomicU64,
}

impl ArbitrageDetector {
    pub fn new(config: ArbitrageConfig) -> Self {
        ArbitrageDetector {
            config,
            opportunities_found: AtomicU64::new(0),
            opportunities_executed: AtomicU64::new(0),
        }
    }

    /// Detect arbitrage opportunities from consolidated book
    pub fn detect(&self, book: &ConsolidatedBook) -> Option<ArbitrageOpportunity> {
        if let Some(mut opp) = book.find_arbitrage() {
            // Filter by minimum profit
            if opp.profit_bps < self.config.min_profit_bps {
                return None;
            }

            // Clamp quantity
            opp.quantity = opp.quantity
                .max(self.config.min_quantity)
                .min(self.config.max_quantity);

            self.opportunities_found.fetch_add(1, Ordering::Relaxed);
            return Some(opp);
        }

        None
    }

    /// Record executed opportunity
    pub fn record_execution(&self, _success: bool) {
        self.opportunities_executed.fetch_add(1, Ordering::Relaxed);
    }

    pub fn opportunities_found(&self) -> u64 {
        self.opportunities_found.load(Ordering::Relaxed)
    }

    pub fn opportunities_executed(&self) -> u64 {
        self.opportunities_executed.load(Ordering::Relaxed)
    }
}

/// Arbitrage executor
pub struct ArbitrageExecutor {
    max_retries: u32,
}

impl ArbitrageExecutor {
    pub fn new() -> Self {
        ArbitrageExecutor { max_retries: 3 }
    }

    /// Execute an arbitrage opportunity
    pub async fn execute(&self, opp: &ArbitrageOpportunity) -> Result<f64, String> {
        tracing::info!(
            "Executing arbitrage: buy on {} @ {}, sell on {} @ {}, profit={:.2} bps",
            opp.buy_exchange,
            from_price(opp.buy_price),
            opp.sell_exchange,
            from_price(opp.sell_price),
            opp.profit_bps
        );

        // In production, would send simultaneous orders to both exchanges
        // For now, return simulated profit
        let profit = from_qty(opp.quantity) * (from_price(opp.sell_price) - from_price(opp.buy_price));
        Ok(profit)
    }
}

impl Default for ArbitrageExecutor {
    fn default() -> Self {
        Self::new()
    }
}
