//! Consolidated order book across multiple exchanges

use crate::core::types::*;
use hashbrown::HashMap;
use parking_lot::RwLock;
use std::cmp::Reverse;
use std::collections::BTreeMap;

/// National Best Bid and Offer across all exchanges
#[derive(Debug, Clone, Copy, Default)]
pub struct NBBO {
    pub best_bid: Price,
    pub best_ask: Price,
    pub best_bid_qty: Quantity,
    pub best_ask_qty: Quantity,
    pub best_bid_exchange: ExchangeId,
    pub best_ask_exchange: ExchangeId,
    pub timestamp: Timestamp,
}

impl NBBO {
    pub fn spread(&self) -> Price {
        self.best_ask - self.best_bid
    }

    pub fn spread_bps(&self) -> f64 {
        let mid = (self.best_bid + self.best_ask) / 2;
        if mid == 0 { 0.0 } else { 10000.0 * self.spread() as f64 / mid as f64 }
    }

    pub fn is_valid(&self) -> bool {
        self.best_bid > 0 && self.best_ask > 0 && self.best_bid < self.best_ask
    }

    /// Check if there's a cross-exchange arbitrage opportunity
    pub fn has_arbitrage(&self) -> bool {
        self.best_bid_exchange != self.best_ask_exchange && self.best_bid >= self.best_ask
    }
}

/// Per-exchange order book
#[derive(Debug, Clone, Default)]
pub struct ExchangeBook {
    pub exchange: ExchangeId,
    pub bids: BTreeMap<Reverse<Price>, Quantity>,
    pub asks: BTreeMap<Price, Quantity>,
    pub last_update: Timestamp,
}

impl ExchangeBook {
    pub fn new(exchange: ExchangeId) -> Self {
        ExchangeBook {
            exchange,
            bids: BTreeMap::new(),
            asks: BTreeMap::new(),
            last_update: 0,
        }
    }

    pub fn best_bid(&self) -> Option<(Price, Quantity)> {
        self.bids.first_key_value().map(|(Reverse(p), q)| (*p, *q))
    }

    pub fn best_ask(&self) -> Option<(Price, Quantity)> {
        self.asks.first_key_value().map(|(p, q)| (*p, *q))
    }

    pub fn update_bid(&mut self, price: Price, qty: Quantity) {
        if qty == 0 {
            self.bids.remove(&Reverse(price));
        } else {
            self.bids.insert(Reverse(price), qty);
        }
        self.last_update = now_nanos();
    }

    pub fn update_ask(&mut self, price: Price, qty: Quantity) {
        if qty == 0 {
            self.asks.remove(&price);
        } else {
            self.asks.insert(price, qty);
        }
        self.last_update = now_nanos();
    }
}

/// Consolidated order book aggregating multiple exchanges
pub struct ConsolidatedBook {
    symbol: Symbol,
    books: RwLock<HashMap<ExchangeId, ExchangeBook>>,
    nbbo: RwLock<NBBO>,
}

impl ConsolidatedBook {
    pub fn new(symbol: Symbol) -> Self {
        ConsolidatedBook {
            symbol,
            books: RwLock::new(HashMap::new()),
            nbbo: RwLock::new(NBBO::default()),
        }
    }

    pub fn update(&self, exchange: ExchangeId, bid: Price, bid_qty: Quantity, ask: Price, ask_qty: Quantity) {
        {
            let mut books = self.books.write();
            let book = books.entry(exchange).or_insert_with(|| ExchangeBook::new(exchange));
            book.update_bid(bid, bid_qty);
            book.update_ask(ask, ask_qty);
        }
        self.recalculate_nbbo();
    }

    fn recalculate_nbbo(&self) {
        let books = self.books.read();
        let mut nbbo = NBBO::default();
        nbbo.timestamp = now_nanos();

        for (exchange, book) in books.iter() {
            if let Some((bid, qty)) = book.best_bid() {
                if bid > nbbo.best_bid {
                    nbbo.best_bid = bid;
                    nbbo.best_bid_qty = qty;
                    nbbo.best_bid_exchange = *exchange;
                }
            }
            if let Some((ask, qty)) = book.best_ask() {
                if nbbo.best_ask == 0 || ask < nbbo.best_ask {
                    nbbo.best_ask = ask;
                    nbbo.best_ask_qty = qty;
                    nbbo.best_ask_exchange = *exchange;
                }
            }
        }

        *self.nbbo.write() = nbbo;
    }

    pub fn get_nbbo(&self) -> NBBO {
        *self.nbbo.read()
    }

    pub fn get_exchange_quote(&self, exchange: ExchangeId) -> Option<ExchangeQuote> {
        let books = self.books.read();
        books.get(&exchange).and_then(|book| {
            let (bid, bid_qty) = book.best_bid()?;
            let (ask, ask_qty) = book.best_ask()?;
            Some(ExchangeQuote {
                exchange,
                bid,
                ask,
                bid_qty,
                ask_qty,
                timestamp: book.last_update,
                latency_ns: 0,
            })
        })
    }

    pub fn find_arbitrage(&self) -> Option<ArbitrageOpportunity> {
        let nbbo = self.get_nbbo();

        // Check if best bid > best ask on different exchanges
        if nbbo.best_bid_exchange != nbbo.best_ask_exchange && nbbo.best_bid > nbbo.best_ask {
            let profit_bps = 10000.0 * (nbbo.best_bid - nbbo.best_ask) as f64 /
                            ((nbbo.best_bid + nbbo.best_ask) / 2) as f64;
            let qty = nbbo.best_bid_qty.min(nbbo.best_ask_qty);

            return Some(ArbitrageOpportunity {
                symbol: self.symbol.clone(),
                buy_exchange: nbbo.best_ask_exchange,
                sell_exchange: nbbo.best_bid_exchange,
                buy_price: nbbo.best_ask,
                sell_price: nbbo.best_bid,
                quantity: qty,
                profit_bps,
                timestamp: now_nanos(),
            });
        }

        None
    }

    pub fn symbol(&self) -> &Symbol {
        &self.symbol
    }
}
