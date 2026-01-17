//! Order book implementation with L2/L3 support

use super::PriceLevel;
use crate::core::types::*;
use hashbrown::HashMap;
use std::cmp::Reverse;
use std::collections::BTreeMap;

/// L2 Order Book - Price level aggregated
pub struct OrderBook {
    symbol: Symbol,

    // Bids: sorted descending (best bid first)
    bids: BTreeMap<Reverse<Price>, PriceLevel>,

    // Asks: sorted ascending (best ask first)
    asks: BTreeMap<Price, PriceLevel>,

    // L3: Individual orders
    orders: HashMap<OrderId, Order>,

    // Cached arrays for fast access
    bid_cache: Vec<PriceLevel>,
    ask_cache: Vec<PriceLevel>,
    cache_dirty: bool,

    last_update: Timestamp,
    sequence: SequenceNum,
}

impl OrderBook {
    pub const MAX_DEPTH: usize = 100;

    pub fn new(symbol: Symbol) -> Self {
        OrderBook {
            symbol,
            bids: BTreeMap::new(),
            asks: BTreeMap::new(),
            orders: HashMap::new(),
            bid_cache: Vec::with_capacity(Self::MAX_DEPTH),
            ask_cache: Vec::with_capacity(Self::MAX_DEPTH),
            cache_dirty: true,
            last_update: 0,
            sequence: 0,
        }
    }

    // ========================================================================
    // L2 Updates
    // ========================================================================

    /// Update bid at price level
    pub fn update_bid(&mut self, price: Price, quantity: Quantity) {
        if quantity == 0 {
            self.bids.remove(&Reverse(price));
        } else {
            self.bids.insert(Reverse(price), PriceLevel::new(price, quantity));
        }
        self.cache_dirty = true;
        self.last_update = now_nanos();
    }

    /// Update ask at price level
    pub fn update_ask(&mut self, price: Price, quantity: Quantity) {
        if quantity == 0 {
            self.asks.remove(&price);
        } else {
            self.asks.insert(price, PriceLevel::new(price, quantity));
        }
        self.cache_dirty = true;
        self.last_update = now_nanos();
    }

    /// Clear all bids
    pub fn clear_bids(&mut self) {
        self.bids.clear();
        self.cache_dirty = true;
    }

    /// Clear all asks
    pub fn clear_asks(&mut self) {
        self.asks.clear();
        self.cache_dirty = true;
    }

    /// Apply full snapshot
    pub fn apply_snapshot(&mut self, bids: Vec<(Price, Quantity)>, asks: Vec<(Price, Quantity)>) {
        self.bids.clear();
        self.asks.clear();

        for (price, qty) in bids {
            self.bids.insert(Reverse(price), PriceLevel::new(price, qty));
        }

        for (price, qty) in asks {
            self.asks.insert(price, PriceLevel::new(price, qty));
        }

        self.cache_dirty = true;
        self.last_update = now_nanos();
    }

    // ========================================================================
    // L3 Updates
    // ========================================================================

    /// Add individual order
    pub fn add_order(&mut self, order: Order) {
        let price = order.price;
        let qty = order.quantity;
        let side = order.side;

        self.orders.insert(order.id, order);

        match side {
            Side::Buy => {
                self.bids
                    .entry(Reverse(price))
                    .and_modify(|level| {
                        level.quantity += qty;
                        level.order_count += 1;
                    })
                    .or_insert_with(|| PriceLevel {
                        price,
                        quantity: qty,
                        order_count: 1,
                        last_update: now_nanos(),
                    });
            }
            Side::Sell => {
                self.asks
                    .entry(price)
                    .and_modify(|level| {
                        level.quantity += qty;
                        level.order_count += 1;
                    })
                    .or_insert_with(|| PriceLevel {
                        price,
                        quantity: qty,
                        order_count: 1,
                        last_update: now_nanos(),
                    });
            }
        }

        self.cache_dirty = true;
    }

    /// Remove individual order
    pub fn remove_order(&mut self, order_id: OrderId) -> Option<Order> {
        if let Some(order) = self.orders.remove(&order_id) {
            let price = order.price;
            let qty = order.remaining();

            match order.side {
                Side::Buy => {
                    if let Some(level) = self.bids.get_mut(&Reverse(price)) {
                        level.quantity -= qty;
                        level.order_count -= 1;
                        if level.quantity <= 0 || level.order_count == 0 {
                            self.bids.remove(&Reverse(price));
                        }
                    }
                }
                Side::Sell => {
                    if let Some(level) = self.asks.get_mut(&price) {
                        level.quantity -= qty;
                        level.order_count -= 1;
                        if level.quantity <= 0 || level.order_count == 0 {
                            self.asks.remove(&price);
                        }
                    }
                }
            }

            self.cache_dirty = true;
            return Some(order);
        }
        None
    }

    // ========================================================================
    // Queries
    // ========================================================================

    /// Get best bid price
    pub fn best_bid(&self) -> Option<Price> {
        self.bids.first_key_value().map(|(Reverse(p), _)| *p)
    }

    /// Get best ask price
    pub fn best_ask(&self) -> Option<Price> {
        self.asks.first_key_value().map(|(p, _)| *p)
    }

    /// Get best bid quantity
    pub fn best_bid_qty(&self) -> Option<Quantity> {
        self.bids.first_key_value().map(|(_, level)| level.quantity)
    }

    /// Get best ask quantity
    pub fn best_ask_qty(&self) -> Option<Quantity> {
        self.asks.first_key_value().map(|(_, level)| level.quantity)
    }

    /// Get mid price
    pub fn mid_price(&self) -> Option<Price> {
        match (self.best_bid(), self.best_ask()) {
            (Some(bid), Some(ask)) => Some((bid + ask) / 2),
            _ => None,
        }
    }

    /// Get spread
    pub fn spread(&self) -> Option<Price> {
        match (self.best_bid(), self.best_ask()) {
            (Some(bid), Some(ask)) => Some(ask - bid),
            _ => None,
        }
    }

    /// Get spread in basis points
    pub fn spread_bps(&self) -> Option<f64> {
        match (self.mid_price(), self.spread()) {
            (Some(mid), Some(spread)) if mid > 0 => {
                Some(10000.0 * (spread as f64) / (mid as f64))
            }
            _ => None,
        }
    }

    /// Get bid level at depth
    pub fn bid_level(&self, depth: usize) -> Option<&PriceLevel> {
        if self.cache_dirty {
            // Can't mutate self here, caller should ensure cache is fresh
            return self.bids.values().nth(depth);
        }
        self.bid_cache.get(depth)
    }

    /// Get ask level at depth
    pub fn ask_level(&self, depth: usize) -> Option<&PriceLevel> {
        if self.cache_dirty {
            return self.asks.values().nth(depth);
        }
        self.ask_cache.get(depth)
    }

    /// Get bid depth (number of levels)
    pub fn bid_depth(&self) -> usize {
        self.bids.len()
    }

    /// Get ask depth (number of levels)
    pub fn ask_depth(&self) -> usize {
        self.asks.len()
    }

    /// Calculate VWAP for buying
    pub fn vwap_ask(&self, target_qty: Quantity) -> Option<Price> {
        let mut remaining = target_qty;
        let mut total_value: i64 = 0;
        let mut total_qty: Quantity = 0;

        for (price, level) in &self.asks {
            let fill_qty = remaining.min(level.quantity);
            total_value += price * fill_qty;
            total_qty += fill_qty;
            remaining -= fill_qty;

            if remaining <= 0 {
                break;
            }
        }

        if total_qty == 0 {
            None
        } else {
            Some(total_value / total_qty)
        }
    }

    /// Calculate VWAP for selling
    pub fn vwap_bid(&self, target_qty: Quantity) -> Option<Price> {
        let mut remaining = target_qty;
        let mut total_value: i64 = 0;
        let mut total_qty: Quantity = 0;

        for (Reverse(price), level) in &self.bids {
            let fill_qty = remaining.min(level.quantity);
            total_value += price * fill_qty;
            total_qty += fill_qty;
            remaining -= fill_qty;

            if remaining <= 0 {
                break;
            }
        }

        if total_qty == 0 {
            None
        } else {
            Some(total_value / total_qty)
        }
    }

    /// Calculate book imbalance (positive = bid heavy)
    pub fn imbalance(&self, levels: usize) -> f64 {
        let bid_vol: Quantity = self.bids.values().take(levels).map(|l| l.quantity).sum();
        let ask_vol: Quantity = self.asks.values().take(levels).map(|l| l.quantity).sum();

        let total = bid_vol + ask_vol;
        if total == 0 {
            return 0.0;
        }

        (bid_vol - ask_vol) as f64 / total as f64
    }

    /// Check if book is valid (not crossed)
    pub fn is_valid(&self) -> bool {
        match (self.best_bid(), self.best_ask()) {
            (Some(bid), Some(ask)) => bid < ask,
            _ => true, // Empty book is valid
        }
    }

    /// Refresh the cache
    pub fn refresh_cache(&mut self) {
        if !self.cache_dirty {
            return;
        }

        self.bid_cache.clear();
        self.ask_cache.clear();

        for (_, level) in self.bids.iter().take(Self::MAX_DEPTH) {
            self.bid_cache.push(*level);
        }

        for (_, level) in self.asks.iter().take(Self::MAX_DEPTH) {
            self.ask_cache.push(*level);
        }

        self.cache_dirty = false;
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    pub fn symbol(&self) -> &Symbol {
        &self.symbol
    }

    pub fn last_update(&self) -> Timestamp {
        self.last_update
    }

    pub fn sequence(&self) -> SequenceNum {
        self.sequence
    }

    pub fn set_sequence(&mut self, seq: SequenceNum) {
        self.sequence = seq;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_orderbook_basic() {
        let mut book = OrderBook::new(Symbol::new("BTCUSDT"));

        book.update_bid(to_price(50000.0), to_qty(1.0));
        book.update_bid(to_price(49999.0), to_qty(2.0));
        book.update_ask(to_price(50001.0), to_qty(1.5));
        book.update_ask(to_price(50002.0), to_qty(2.5));

        assert_eq!(book.best_bid(), Some(to_price(50000.0)));
        assert_eq!(book.best_ask(), Some(to_price(50001.0)));
        assert_eq!(book.spread(), Some(to_price(1.0)));
    }

    #[test]
    fn test_orderbook_imbalance() {
        let mut book = OrderBook::new(Symbol::new("BTCUSDT"));

        book.update_bid(to_price(100.0), to_qty(10.0));
        book.update_ask(to_price(101.0), to_qty(5.0));

        let imbalance = book.imbalance(1);
        assert!(imbalance > 0.0); // More bids than asks
    }

    #[test]
    fn test_orderbook_vwap() {
        let mut book = OrderBook::new(Symbol::new("BTCUSDT"));

        book.update_ask(to_price(100.0), to_qty(1.0));
        book.update_ask(to_price(101.0), to_qty(1.0));
        book.update_ask(to_price(102.0), to_qty(1.0));

        // VWAP to buy 2 units: (100*1 + 101*1) / 2 = 100.5
        let vwap = book.vwap_ask(to_qty(2.0)).unwrap();
        let expected = to_price(100.5);
        assert!((vwap - expected).abs() < to_price(0.01));
    }
}
