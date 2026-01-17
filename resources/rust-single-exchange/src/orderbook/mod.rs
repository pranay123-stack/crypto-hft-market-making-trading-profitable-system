//! Order book implementation

use crate::core::types::*;
use hashbrown::HashMap;
use ordered_float::OrderedFloat;
use std::collections::BTreeMap;

mod book;

pub use book::OrderBook;

/// A price level in the order book
#[derive(Debug, Clone, Copy, Default)]
pub struct PriceLevel {
    pub price: Price,
    pub quantity: Quantity,
    pub order_count: u32,
    pub last_update: Timestamp,
}

impl PriceLevel {
    pub fn new(price: Price, quantity: Quantity) -> Self {
        PriceLevel {
            price,
            quantity,
            order_count: 1,
            last_update: now_nanos(),
        }
    }
}
