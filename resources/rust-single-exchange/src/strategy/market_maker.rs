//! Market making strategy implementations

use super::{MarketMaker, MarketMakerParams, QuoteDecision, Signal};
use crate::core::types::*;
use crate::orderbook::OrderBook;

/// Basic market making strategy
pub struct BasicMarketMaker {
    params: MarketMakerParams,
    enabled: bool,

    // Active quotes
    active_bid_id: OrderId,
    active_ask_id: OrderId,
    active_bid_price: Price,
    active_ask_price: Price,

    // Statistics
    quotes_sent: u64,
    fills: u64,
    last_quote_time: Timestamp,
}

impl BasicMarketMaker {
    pub fn new(params: MarketMakerParams) -> Self {
        BasicMarketMaker {
            params,
            enabled: false,
            active_bid_id: 0,
            active_ask_id: 0,
            active_bid_price: 0,
            active_ask_price: 0,
            quotes_sent: 0,
            fills: 0,
            last_quote_time: 0,
        }
    }

    fn calculate_fair_value(&self, book: &OrderBook) -> Option<Price> {
        book.mid_price()
    }

    fn calculate_spread(&self, _book: &OrderBook, signal: &Signal) -> f64 {
        let mut spread = self.params.target_spread_bps;

        // Adjust for volatility
        if signal.volatility > 0.0 {
            spread *= 1.0 + signal.volatility;
        }

        spread.clamp(self.params.min_spread_bps, self.params.max_spread_bps)
    }

    fn calculate_inventory_skew(&self, position: Quantity) -> f64 {
        if self.params.max_position == 0 {
            return 0.0;
        }
        position as f64 / self.params.max_position as f64
    }

    fn calculate_order_size(&self, side: Side, position: Quantity) -> Quantity {
        let mut size = self.params.default_order_size;

        if self.params.max_position > 0 {
            match side {
                Side::Buy if position > 0 => {
                    // Already long, reduce buy size
                    let ratio = 1.0 - (position as f64 / self.params.max_position as f64);
                    size = (size as f64 * ratio.max(0.0)) as Quantity;
                }
                Side::Sell if position < 0 => {
                    // Already short, reduce sell size
                    let ratio = 1.0 + (position as f64 / self.params.max_position as f64);
                    size = (size as f64 * ratio.max(0.0)) as Quantity;
                }
                _ => {}
            }
        }

        size.clamp(self.params.min_order_size, self.params.max_order_size)
    }
}

impl MarketMaker for BasicMarketMaker {
    fn compute_quotes(
        &mut self,
        book: &OrderBook,
        position: Quantity,
        signal: &Signal,
    ) -> QuoteDecision {
        let mut decision = QuoteDecision::default();

        if !self.enabled {
            decision.reason = "Strategy disabled".to_string();
            return decision;
        }

        if !book.is_valid() {
            decision.reason = "Invalid orderbook".to_string();
            return decision;
        }

        let fair_value = match self.calculate_fair_value(book) {
            Some(fv) => fv,
            None => {
                decision.reason = "Cannot determine fair value".to_string();
                return decision;
            }
        };

        // Calculate spread
        let spread_bps = self.calculate_spread(book, signal);
        let half_spread = (fair_value as f64 * spread_bps / 20000.0) as Price;

        // Calculate inventory skew
        let skew = self.calculate_inventory_skew(position);
        let skew_adjustment =
            (fair_value as f64 * skew * self.params.inventory_skew / 10000.0) as Price;

        decision.bid_price = fair_value - half_spread - skew_adjustment;
        decision.ask_price = fair_value + half_spread - skew_adjustment;

        // Ensure no crossing
        if decision.bid_price >= decision.ask_price {
            decision.reason = "Prices would cross".to_string();
            return decision;
        }

        // Calculate sizes
        decision.bid_size = self.calculate_order_size(Side::Buy, position);
        decision.ask_size = self.calculate_order_size(Side::Sell, position);

        if decision.bid_size == 0 && decision.ask_size == 0 {
            decision.reason = "Order sizes are zero".to_string();
            return decision;
        }

        // Check if we should skip quoting
        let now = now_nanos();
        if now - self.last_quote_time < self.params.min_quote_life_us * 1000 {
            let bid_diff = (decision.bid_price - self.active_bid_price).abs();
            let ask_diff = (decision.ask_price - self.active_ask_price).abs();
            let threshold = fair_value / 10000; // 1 bps

            if bid_diff < threshold && ask_diff < threshold {
                decision.reason = "Prices unchanged".to_string();
                return decision;
            }
        }

        decision.should_quote = true;
        self.last_quote_time = now;
        self.quotes_sent += 1;

        decision
    }

    fn on_fill(&mut self, _order: &Order, _filled_qty: Quantity, _fill_price: Price) {
        self.fills += 1;
    }

    fn on_cancel(&mut self, order_id: OrderId) {
        if order_id == self.active_bid_id {
            self.active_bid_id = 0;
            self.active_bid_price = 0;
        } else if order_id == self.active_ask_id {
            self.active_ask_id = 0;
            self.active_ask_price = 0;
        }
    }

    fn params(&self) -> &MarketMakerParams {
        &self.params
    }

    fn update_params(&mut self, params: MarketMakerParams) {
        self.params = params;
    }

    fn set_enabled(&mut self, enabled: bool) {
        self.enabled = enabled;
    }

    fn is_enabled(&self) -> bool {
        self.enabled
    }
}

/// Avellaneda-Stoikov optimal market making strategy
pub struct AvellanedaStoikovMM {
    base: BasicMarketMaker,

    // A-S specific parameters
    gamma: f64,  // Risk aversion
    sigma: f64,  // Volatility
    k: f64,      // Order arrival intensity
    t_horizon: f64,  // Time horizon

    start_time: Timestamp,
}

impl AvellanedaStoikovMM {
    pub fn new(params: MarketMakerParams, gamma: f64, sigma: f64, k: f64, t_horizon: f64) -> Self {
        AvellanedaStoikovMM {
            base: BasicMarketMaker::new(params),
            gamma,
            sigma,
            k,
            t_horizon,
            start_time: 0,
        }
    }

    fn calculate_reservation_price(&self, mid: Price, position: Quantity, t_remaining: f64) -> Price {
        // r(s,q,t) = s - q * gamma * sigma^2 * (T - t)
        let adjustment = position as f64 * self.gamma * self.sigma.powi(2) * t_remaining;
        mid - (mid as f64 * adjustment) as Price
    }

    fn calculate_optimal_spread(&self, t_remaining: f64) -> f64 {
        // delta = gamma * sigma^2 * (T - t) + (2/gamma) * ln(1 + gamma/k)
        let term1 = self.gamma * self.sigma.powi(2) * t_remaining;
        let term2 = (2.0 / self.gamma) * (1.0 + self.gamma / self.k).ln();
        (term1 + term2) * 10000.0 // Convert to bps
    }
}

impl MarketMaker for AvellanedaStoikovMM {
    fn compute_quotes(
        &mut self,
        book: &OrderBook,
        position: Quantity,
        signal: &Signal,
    ) -> QuoteDecision {
        let mut decision = QuoteDecision::default();

        if !self.base.enabled || !book.is_valid() {
            decision.reason = "Disabled or invalid book".to_string();
            return decision;
        }

        // Initialize start time
        if self.start_time == 0 {
            self.start_time = signal.timestamp;
        }

        // Calculate time remaining
        let elapsed_secs = (signal.timestamp - self.start_time) as f64 / 1e9;
        let t_elapsed = elapsed_secs / self.t_horizon;
        let t_remaining = (1.0 - (t_elapsed % 1.0)).max(0.01);

        let mid = match book.mid_price() {
            Some(m) => m,
            None => {
                decision.reason = "No mid price".to_string();
                return decision;
            }
        };

        // Calculate reservation price
        let reservation = self.calculate_reservation_price(mid, position, t_remaining);

        // Calculate optimal spread
        let spread_bps = self.calculate_optimal_spread(t_remaining)
            .clamp(self.base.params.min_spread_bps, self.base.params.max_spread_bps);
        let half_spread = (mid as f64 * spread_bps / 20000.0) as Price;

        decision.bid_price = reservation - half_spread;
        decision.ask_price = reservation + half_spread;

        if decision.bid_price >= decision.ask_price {
            decision.reason = "Prices would cross".to_string();
            return decision;
        }

        decision.bid_size = self.base.calculate_order_size(Side::Buy, position);
        decision.ask_size = self.base.calculate_order_size(Side::Sell, position);

        if decision.bid_size > 0 || decision.ask_size > 0 {
            decision.should_quote = true;
        }

        decision
    }

    fn on_fill(&mut self, order: &Order, filled_qty: Quantity, fill_price: Price) {
        self.base.on_fill(order, filled_qty, fill_price);
    }

    fn on_cancel(&mut self, order_id: OrderId) {
        self.base.on_cancel(order_id);
    }

    fn params(&self) -> &MarketMakerParams {
        self.base.params()
    }

    fn update_params(&mut self, params: MarketMakerParams) {
        self.base.update_params(params);
    }

    fn set_enabled(&mut self, enabled: bool) {
        self.base.set_enabled(enabled);
    }

    fn is_enabled(&self) -> bool {
        self.base.is_enabled()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_mm_quotes() {
        let params = MarketMakerParams::default();
        let mut mm = BasicMarketMaker::new(params);
        mm.set_enabled(true);

        let mut book = OrderBook::new(Symbol::new("BTCUSDT"));
        book.update_bid(to_price(50000.0), to_qty(1.0));
        book.update_ask(to_price(50001.0), to_qty(1.0));

        let signal = Signal::default();
        let decision = mm.compute_quotes(&book, 0, &signal);

        assert!(decision.should_quote);
        assert!(decision.bid_price < to_price(50000.0));
        assert!(decision.ask_price > to_price(50001.0));
    }
}
