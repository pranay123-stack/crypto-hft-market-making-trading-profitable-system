//! Multi-exchange management

use crate::core::types::*;
use async_trait::async_trait;
use dashmap::DashMap;
use std::sync::Arc;
use thiserror::Error;

pub mod binance;
pub mod bybit;

pub use crate::core::types::ExchangeId;

#[derive(Debug, Error)]
pub enum ExchangeError {
    #[error("Connection failed: {0}")]
    ConnectionFailed(String),
    #[error("Request failed: {0}")]
    RequestFailed(String),
    #[error("Not connected")]
    NotConnected,
}

/// Exchange client trait
#[async_trait]
pub trait ExchangeClient: Send + Sync {
    fn id(&self) -> ExchangeId;
    fn name(&self) -> &str;
    async fn connect(&mut self) -> Result<(), ExchangeError>;
    async fn disconnect(&mut self) -> Result<(), ExchangeError>;
    fn is_connected(&self) -> bool;
    async fn subscribe(&mut self, symbol: &Symbol) -> Result<(), ExchangeError>;
    async fn send_order(&self, order: &Order) -> Result<OrderId, ExchangeError>;
    async fn cancel_order(&self, order_id: OrderId) -> Result<(), ExchangeError>;
    fn get_latency(&self) -> Timestamp;
}

/// Manages multiple exchange connections
pub struct ExchangeManager {
    clients: DashMap<ExchangeId, Arc<dyn ExchangeClient>>,
}

impl ExchangeManager {
    pub fn new() -> Self {
        ExchangeManager {
            clients: DashMap::new(),
        }
    }

    pub fn add_client(&self, client: Arc<dyn ExchangeClient>) {
        self.clients.insert(client.id(), client);
    }

    pub fn get_client(&self, id: ExchangeId) -> Option<Arc<dyn ExchangeClient>> {
        self.clients.get(&id).map(|c| c.clone())
    }

    pub async fn connect_all(&self) -> Result<(), ExchangeError> {
        for mut client in self.clients.iter_mut() {
            // Note: Would need interior mutability pattern for real implementation
            tracing::info!("Connecting to {}", client.name());
        }
        Ok(())
    }

    pub fn connected_exchanges(&self) -> Vec<ExchangeId> {
        self.clients
            .iter()
            .filter(|c| c.is_connected())
            .map(|c| c.id())
            .collect()
    }

    /// Get fastest exchange by latency
    pub fn fastest_exchange(&self) -> Option<ExchangeId> {
        self.clients
            .iter()
            .filter(|c| c.is_connected())
            .min_by_key(|c| c.get_latency())
            .map(|c| c.id())
    }

    /// Route order to best exchange
    pub fn select_exchange_for_order(&self, side: Side, quotes: &[(ExchangeId, ExchangeQuote)]) -> Option<ExchangeId> {
        quotes
            .iter()
            .filter(|(id, _)| self.clients.get(id).map(|c| c.is_connected()).unwrap_or(false))
            .min_by_key(|(_, q)| match side {
                Side::Buy => q.ask,   // Best (lowest) ask for buying
                Side::Sell => -q.bid, // Best (highest) bid for selling (negate for min)
            })
            .map(|(id, _)| *id)
    }
}

impl Default for ExchangeManager {
    fn default() -> Self {
        Self::new()
    }
}
