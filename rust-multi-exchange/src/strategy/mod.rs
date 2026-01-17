//! Cross-exchange market making strategy

use crate::core::types::*;
use crate::orderbook::{ConsolidatedBook, NBBO};
use hashbrown::HashMap;

/// Cross-exchange position tracking
#[derive(Debug, Clone, Default)]
pub struct CrossExchangePosition {
    pub positions: HashMap<ExchangeId, Quantity>,
    pub net_position: Quantity,
}

impl CrossExchangePosition {
    pub fn update(&mut self, exchange: ExchangeId, qty: Quantity) {
        self.positions.insert(exchange, qty);
        self.net_position = self.positions.values().sum();
    }

    pub fn get(&self, exchange: ExchangeId) -> Quantity {
        *self.positions.get(&exchange).unwrap_or(&0)
    }
}

/// Quote decision for multiple exchanges
#[derive(Debug, Clone, Default)]
pub struct MultiExchangeQuotes {
    pub quotes: Vec<(ExchangeId, Price, Price, Quantity, Quantity)>, // (exchange, bid, ask, bid_size, ask_size)
    pub should_quote: bool,
}

/// Cross-exchange market making parameters
#[derive(Debug, Clone)]
pub struct CrossExchangeMMParams {
    pub target_spread_bps: f64,
    pub max_position_per_exchange: Quantity,
    pub max_total_position: Quantity,
    pub default_order_size: Quantity,
    pub hedge_immediately: bool,
    pub quote_exchanges: Vec<ExchangeId>,
    pub hedge_exchanges: Vec<ExchangeId>,
}

impl Default for CrossExchangeMMParams {
    fn default() -> Self {
        CrossExchangeMMParams {
            target_spread_bps: 15.0,
            max_position_per_exchange: to_qty(0.1),
            max_total_position: to_qty(0.2),
            default_order_size: to_qty(0.001),
            hedge_immediately: true,
            quote_exchanges: vec![ExchangeId::Binance, ExchangeId::Bybit],
            hedge_exchanges: vec![ExchangeId::Binance, ExchangeId::Bybit],
        }
    }
}

/// Cross-exchange market maker
pub struct CrossExchangeMM {
    params: CrossExchangeMMParams,
    enabled: bool,
    quotes_sent: u64,
    fills: u64,
}

impl CrossExchangeMM {
    pub fn new(params: CrossExchangeMMParams) -> Self {
        CrossExchangeMM {
            params,
            enabled: false,
            quotes_sent: 0,
            fills: 0,
        }
    }

    pub fn compute_quotes(&mut self, book: &ConsolidatedBook, position: &CrossExchangePosition) -> MultiExchangeQuotes {
        let mut result = MultiExchangeQuotes::default();

        if !self.enabled {
            return result;
        }

        let nbbo = book.get_nbbo();
        if !nbbo.is_valid() {
            return result;
        }

        let fair_value = (nbbo.best_bid + nbbo.best_ask) / 2;
        let half_spread = (fair_value as f64 * self.params.target_spread_bps / 20000.0) as Price;

        // Compute quotes for each exchange
        for exchange in &self.params.quote_exchanges {
            let pos = position.get(*exchange);

            // Skip if at position limit
            if pos.abs() >= self.params.max_position_per_exchange {
                continue;
            }

            // Inventory skew
            let skew = if self.params.max_position_per_exchange > 0 {
                pos as f64 / self.params.max_position_per_exchange as f64
            } else {
                0.0
            };
            let skew_adj = (fair_value as f64 * skew * 0.5 / 10000.0) as Price;

            let bid_price = fair_value - half_spread - skew_adj;
            let ask_price = fair_value + half_spread - skew_adj;

            let bid_size = self.params.default_order_size;
            let ask_size = self.params.default_order_size;

            result.quotes.push((*exchange, bid_price, ask_price, bid_size, ask_size));
        }

        if !result.quotes.is_empty() {
            result.should_quote = true;
            self.quotes_sent += result.quotes.len() as u64;
        }

        result
    }

    /// Compute hedge order after a fill
    pub fn compute_hedge(&self, fill_exchange: ExchangeId, fill_side: Side, fill_qty: Quantity, book: &ConsolidatedBook) -> Option<(ExchangeId, Order)> {
        if !self.params.hedge_immediately {
            return None;
        }

        // Find best exchange to hedge on (not the fill exchange)
        let nbbo = book.get_nbbo();
        let hedge_side = match fill_side {
            Side::Buy => Side::Sell,
            Side::Sell => Side::Buy,
        };

        let hedge_exchange = match hedge_side {
            Side::Sell => {
                if nbbo.best_bid_exchange != fill_exchange {
                    nbbo.best_bid_exchange
                } else {
                    // Find second best
                    *self.params.hedge_exchanges.iter()
                        .find(|e| **e != fill_exchange)
                        .unwrap_or(&fill_exchange)
                }
            }
            Side::Buy => {
                if nbbo.best_ask_exchange != fill_exchange {
                    nbbo.best_ask_exchange
                } else {
                    *self.params.hedge_exchanges.iter()
                        .find(|e| **e != fill_exchange)
                        .unwrap_or(&fill_exchange)
                }
            }
        };

        let hedge_price = match hedge_side {
            Side::Buy => nbbo.best_ask,
            Side::Sell => nbbo.best_bid,
        };

        let order = Order {
            id: 0,
            exchange: hedge_exchange,
            symbol: book.symbol().clone(),
            side: hedge_side,
            price: hedge_price,
            quantity: fill_qty,
            filled_qty: 0,
            status: OrderStatus::New,
            timestamp: now_nanos(),
        };

        Some((hedge_exchange, order))
    }

    pub fn enable(&mut self) { self.enabled = true; }
    pub fn disable(&mut self) { self.enabled = false; }
    pub fn is_enabled(&self) -> bool { self.enabled }
    pub fn quotes_sent(&self) -> u64 { self.quotes_sent }
    pub fn fills(&self) -> u64 { self.fills }

    pub fn on_fill(&mut self) {
        self.fills += 1;
    }
}
