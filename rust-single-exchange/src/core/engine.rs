//! Trading Engine - Core system orchestrator

use crate::core::types::*;
use crate::exchange::ExchangeClient;
use crate::orderbook::OrderBook;
use crate::risk::{RiskManager, RiskLimits};
use crate::strategy::{MarketMaker, MarketMakerParams, QuoteDecision, Signal};

use crossbeam_channel::{bounded, Receiver, Sender};
use parking_lot::RwLock;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use tokio::sync::mpsc;
use tracing::{debug, error, info, warn};

// ============================================================================
// Engine Configuration
// ============================================================================

#[derive(Debug, Clone)]
pub struct EngineConfig {
    pub symbol: Symbol,
    pub exchange: String,
    pub strategy: MarketMakerParams,
    pub risk: RiskLimits,

    // System settings
    pub tick_buffer_size: usize,
    pub order_buffer_size: usize,
    pub enable_trading: bool,
}

impl Default for EngineConfig {
    fn default() -> Self {
        EngineConfig {
            symbol: Symbol::new("BTCUSDT"),
            exchange: "binance".to_string(),
            strategy: MarketMakerParams::default(),
            risk: RiskLimits::default(),
            tick_buffer_size: 65536,
            order_buffer_size: 8192,
            enable_trading: false,
        }
    }
}

// ============================================================================
// Internal Events
// ============================================================================

#[derive(Debug, Clone)]
pub enum EngineEvent {
    Tick(Tick),
    OrderUpdate(Order),
    Trade(Trade),
    Shutdown,
}

// ============================================================================
// Trading Engine
// ============================================================================

pub struct TradingEngine {
    config: EngineConfig,

    // Core components
    orderbook: Arc<RwLock<OrderBook>>,
    strategy: Arc<RwLock<Box<dyn MarketMaker + Send + Sync>>>,
    risk_manager: Arc<RwLock<RiskManager>>,

    // State
    running: Arc<AtomicBool>,
    trading_enabled: Arc<AtomicBool>,
    order_id_counter: Arc<AtomicU64>,

    // Statistics
    ticks_processed: Arc<AtomicU64>,
    orders_sent: Arc<AtomicU64>,
    trades_executed: Arc<AtomicU64>,

    // Channels for internal communication
    event_tx: Sender<EngineEvent>,
    event_rx: Receiver<EngineEvent>,
}

impl TradingEngine {
    pub fn new(config: EngineConfig) -> Self {
        let (event_tx, event_rx) = bounded(config.tick_buffer_size);

        let orderbook = Arc::new(RwLock::new(OrderBook::new(config.symbol.clone())));
        let risk_manager = Arc::new(RwLock::new(RiskManager::new(config.risk.clone())));

        // Create default strategy
        let strategy: Box<dyn MarketMaker + Send + Sync> =
            Box::new(crate::strategy::BasicMarketMaker::new(config.strategy.clone()));
        let strategy = Arc::new(RwLock::new(strategy));

        TradingEngine {
            config,
            orderbook,
            strategy,
            risk_manager,
            running: Arc::new(AtomicBool::new(false)),
            trading_enabled: Arc::new(AtomicBool::new(false)),
            order_id_counter: Arc::new(AtomicU64::new(1)),
            ticks_processed: Arc::new(AtomicU64::new(0)),
            orders_sent: Arc::new(AtomicU64::new(0)),
            trades_executed: Arc::new(AtomicU64::new(0)),
            event_tx,
            event_rx,
        }
    }

    pub fn with_strategy<S: MarketMaker + Send + Sync + 'static>(mut self, strategy: S) -> Self {
        self.strategy = Arc::new(RwLock::new(Box::new(strategy)));
        self
    }

    /// Start the trading engine
    pub async fn start(&self) -> anyhow::Result<()> {
        if self.running.load(Ordering::Relaxed) {
            warn!("Engine already running");
            return Ok(());
        }

        info!("Starting trading engine for {}", self.config.symbol);
        self.running.store(true, Ordering::Relaxed);

        // Spawn event processing task
        let event_rx = self.event_rx.clone();
        let orderbook = self.orderbook.clone();
        let strategy = self.strategy.clone();
        let risk_manager = self.risk_manager.clone();
        let running = self.running.clone();
        let trading_enabled = self.trading_enabled.clone();
        let ticks_processed = self.ticks_processed.clone();
        let trades_executed = self.trades_executed.clone();

        tokio::spawn(async move {
            while running.load(Ordering::Relaxed) {
                match event_rx.try_recv() {
                    Ok(event) => {
                        match event {
                            EngineEvent::Tick(tick) => {
                                // Update orderbook
                                {
                                    let mut book = orderbook.write();
                                    book.update_bid(tick.bid, tick.bid_qty);
                                    book.update_ask(tick.ask, tick.ask_qty);
                                }
                                ticks_processed.fetch_add(1, Ordering::Relaxed);

                                // Update risk manager mark price
                                {
                                    let book = orderbook.read();
                                    let mut rm = risk_manager.write();
                                    if let Some(mid) = book.mid_price() {
                                        // rm.update_mark_price(mid);
                                    }
                                }

                                // Compute quotes if trading enabled
                                if trading_enabled.load(Ordering::Relaxed) {
                                    let book = orderbook.read();
                                    let rm = risk_manager.read();
                                    let position = rm.get_position();

                                    let signal = Signal {
                                        fair_value: book.mid_price().map(from_price).unwrap_or(0.0),
                                        volatility: 0.0,
                                        momentum: 0.0,
                                        inventory_pressure: 0.0,
                                        timestamp: now_nanos(),
                                    };

                                    let mut strat = strategy.write();
                                    let decision = strat.compute_quotes(&book, position, &signal);

                                    if decision.should_quote {
                                        debug!(
                                            "Quote decision: bid={} ask={}",
                                            from_price(decision.bid_price),
                                            from_price(decision.ask_price)
                                        );
                                        // Would send orders here
                                    }
                                }
                            }
                            EngineEvent::OrderUpdate(order) => {
                                debug!("Order update: {:?}", order.status);
                            }
                            EngineEvent::Trade(trade) => {
                                trades_executed.fetch_add(1, Ordering::Relaxed);
                                info!(
                                    "Trade executed: {} {} @ {} qty={}",
                                    trade.symbol,
                                    trade.side,
                                    from_price(trade.price),
                                    from_qty(trade.quantity)
                                );
                            }
                            EngineEvent::Shutdown => {
                                info!("Received shutdown event");
                                running.store(false, Ordering::Relaxed);
                                break;
                            }
                        }
                    }
                    Err(crossbeam_channel::TryRecvError::Empty) => {
                        tokio::time::sleep(tokio::time::Duration::from_micros(10)).await;
                    }
                    Err(crossbeam_channel::TryRecvError::Disconnected) => {
                        break;
                    }
                }
            }
        });

        if self.config.enable_trading {
            self.trading_enabled.store(true, Ordering::Relaxed);
        }

        info!("Trading engine started");
        Ok(())
    }

    /// Stop the trading engine
    pub async fn stop(&self) {
        if !self.running.load(Ordering::Relaxed) {
            return;
        }

        info!("Stopping trading engine...");

        // Disable trading first
        self.trading_enabled.store(false, Ordering::Relaxed);

        // Send shutdown event
        let _ = self.event_tx.send(EngineEvent::Shutdown);

        self.running.store(false, Ordering::Relaxed);

        info!(
            "Engine stopped. Stats: ticks={}, orders={}, trades={}",
            self.ticks_processed.load(Ordering::Relaxed),
            self.orders_sent.load(Ordering::Relaxed),
            self.trades_executed.load(Ordering::Relaxed)
        );
    }

    /// Process incoming market data tick
    pub fn on_tick(&self, tick: Tick) {
        let _ = self.event_tx.try_send(EngineEvent::Tick(tick));
    }

    /// Process order update
    pub fn on_order_update(&self, order: Order) {
        let _ = self.event_tx.try_send(EngineEvent::OrderUpdate(order));
    }

    /// Process trade
    pub fn on_trade(&self, trade: Trade) {
        let _ = self.event_tx.try_send(EngineEvent::Trade(trade));
    }

    /// Enable trading
    pub fn enable_trading(&self) {
        info!("Trading enabled");
        self.trading_enabled.store(true, Ordering::Relaxed);
    }

    /// Disable trading
    pub fn disable_trading(&self) {
        info!("Trading disabled");
        self.trading_enabled.store(false, Ordering::Relaxed);
    }

    /// Check if engine is running
    pub fn is_running(&self) -> bool {
        self.running.load(Ordering::Relaxed)
    }

    /// Check if trading is enabled
    pub fn is_trading_enabled(&self) -> bool {
        self.trading_enabled.load(Ordering::Relaxed)
    }

    /// Get next order ID
    pub fn next_order_id(&self) -> OrderId {
        self.order_id_counter.fetch_add(1, Ordering::Relaxed)
    }

    /// Get current orderbook
    pub fn orderbook(&self) -> &Arc<RwLock<OrderBook>> {
        &self.orderbook
    }

    /// Get risk manager
    pub fn risk_manager(&self) -> &Arc<RwLock<RiskManager>> {
        &self.risk_manager
    }

    /// Get statistics
    pub fn stats(&self) -> EngineStats {
        EngineStats {
            ticks_processed: self.ticks_processed.load(Ordering::Relaxed),
            orders_sent: self.orders_sent.load(Ordering::Relaxed),
            trades_executed: self.trades_executed.load(Ordering::Relaxed),
        }
    }
}

#[derive(Debug, Clone)]
pub struct EngineStats {
    pub ticks_processed: u64,
    pub orders_sent: u64,
    pub trades_executed: u64,
}

// ============================================================================
// Engine Builder
// ============================================================================

pub struct EngineBuilder {
    config: EngineConfig,
}

impl EngineBuilder {
    pub fn new() -> Self {
        EngineBuilder {
            config: EngineConfig::default(),
        }
    }

    pub fn symbol(mut self, symbol: impl Into<Symbol>) -> Self {
        self.config.symbol = symbol.into();
        self
    }

    pub fn exchange(mut self, exchange: impl Into<String>) -> Self {
        self.config.exchange = exchange.into();
        self
    }

    pub fn strategy_params(mut self, params: MarketMakerParams) -> Self {
        self.config.strategy = params;
        self
    }

    pub fn risk_limits(mut self, limits: RiskLimits) -> Self {
        self.config.risk = limits;
        self
    }

    pub fn enable_trading(mut self, enabled: bool) -> Self {
        self.config.enable_trading = enabled;
        self
    }

    pub fn build(self) -> TradingEngine {
        TradingEngine::new(self.config)
    }
}

impl Default for EngineBuilder {
    fn default() -> Self {
        Self::new()
    }
}
