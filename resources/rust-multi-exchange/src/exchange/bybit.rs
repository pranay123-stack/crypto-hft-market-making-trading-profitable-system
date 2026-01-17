//! Bybit client for multi-exchange system

use super::{ExchangeClient, ExchangeError};
use crate::core::types::*;
use async_trait::async_trait;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

pub struct BybitClient {
    connected: AtomicBool,
    latency_ns: AtomicU64,
    testnet: bool,
}

impl BybitClient {
    pub fn new(testnet: bool) -> Self {
        BybitClient {
            connected: AtomicBool::new(false),
            latency_ns: AtomicU64::new(0),
            testnet,
        }
    }
}

#[async_trait]
impl ExchangeClient for BybitClient {
    fn id(&self) -> ExchangeId {
        ExchangeId::Bybit
    }

    fn name(&self) -> &str {
        "Bybit"
    }

    async fn connect(&mut self) -> Result<(), ExchangeError> {
        tracing::info!("Connecting to Bybit (testnet={})", self.testnet);
        self.connected.store(true, Ordering::Relaxed);
        Ok(())
    }

    async fn disconnect(&mut self) -> Result<(), ExchangeError> {
        self.connected.store(false, Ordering::Relaxed);
        Ok(())
    }

    fn is_connected(&self) -> bool {
        self.connected.load(Ordering::Relaxed)
    }

    async fn subscribe(&mut self, symbol: &Symbol) -> Result<(), ExchangeError> {
        tracing::info!("Subscribing to {} on Bybit", symbol);
        Ok(())
    }

    async fn send_order(&self, order: &Order) -> Result<OrderId, ExchangeError> {
        if !self.is_connected() {
            return Err(ExchangeError::NotConnected);
        }
        Ok(order.id)
    }

    async fn cancel_order(&self, order_id: OrderId) -> Result<(), ExchangeError> {
        if !self.is_connected() {
            return Err(ExchangeError::NotConnected);
        }
        tracing::debug!("Canceling order {} on Bybit", order_id);
        Ok(())
    }

    fn get_latency(&self) -> Timestamp {
        self.latency_ns.load(Ordering::Relaxed)
    }
}
