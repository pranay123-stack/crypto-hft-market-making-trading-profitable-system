//! Crypto HFT Market Making Bot - Single Exchange
//!
//! A high-performance market making system for cryptocurrency exchanges.

use clap::Parser;
use hft::prelude::*;
use hft::core::engine::{EngineBuilder, TradingEngine};
use hft::exchange::{BinanceClient, ExchangeCallbacks, ExchangeClient};
use hft::exchange::binance::BinanceConfig;
use hft::utils::{init_logging, AppConfig};
use std::sync::Arc;
use tokio::signal;
use tracing::{error, info, warn};

#[derive(Parser, Debug)]
#[command(name = "market-maker")]
#[command(about = "Crypto HFT Market Making Bot", long_about = None)]
struct Args {
    /// Configuration file path
    #[arg(short, long, default_value = "config/config.json")]
    config: String,

    /// Trading symbol
    #[arg(short, long)]
    symbol: Option<String>,

    /// Use testnet
    #[arg(short, long)]
    testnet: bool,

    /// Paper trading mode
    #[arg(short, long)]
    paper: bool,

    /// Verbose logging
    #[arg(short, long)]
    verbose: bool,
}

fn print_banner() {
    println!(r#"
╔═══════════════════════════════════════════════════════════════╗
║          Crypto HFT Market Making System v1.0.0               ║
║          High-Frequency Trading Bot - Single Exchange         ║
║                        (Rust Edition)                         ║
╚═══════════════════════════════════════════════════════════════╝
"#);
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    print_banner();

    let args = Args::parse();

    // Load configuration
    let mut config = match AppConfig::load(&args.config) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("Warning: Failed to load config file: {}. Using defaults.", e);
            AppConfig::default()
        }
    };

    // Apply command line overrides
    if let Some(symbol) = args.symbol {
        config.trading.symbol = symbol;
    }
    if args.testnet || args.paper {
        config.trading.paper_trading = true;
    }
    if args.verbose {
        config.system.log_level = "DEBUG".to_string();
    }

    // Apply environment variable overrides
    config.apply_env_overrides();

    // Initialize logging
    init_logging(
        &config.system.log_level,
        config.system.log_to_file,
        &config.system.log_dir,
    );

    info!("Starting HFT Market Making Bot");
    info!("Configuration loaded from: {}", args.config);
    info!("Trading symbol: {}", config.trading.symbol);
    info!("Mode: {}", if config.trading.paper_trading { "Paper Trading" } else { "LIVE TRADING" });

    // Build trading engine
    let symbol = Symbol::new(&config.trading.symbol);

    let engine = EngineBuilder::new()
        .symbol(symbol.clone())
        .exchange(&config.exchange.name)
        .strategy_params(config.to_strategy_params())
        .risk_limits(config.to_risk_limits())
        .enable_trading(!config.trading.paper_trading)
        .build();

    // Create exchange client
    let binance_config = if config.trading.paper_trading {
        let mut cfg = BinanceConfig::testnet();
        cfg.base.api_key = config.exchange.api_key.clone();
        cfg.base.api_secret = config.exchange.api_secret.clone();
        cfg
    } else {
        BinanceConfig {
            base: config.to_exchange_config(),
            use_futures: false,
            recv_window: 5000,
        }
    };

    let mut client = BinanceClient::new(binance_config);

    // Setup callbacks
    let engine_ref = Arc::new(engine);
    let engine_clone = engine_ref.clone();

    client.set_callbacks(ExchangeCallbacks {
        on_tick: Some(Box::new(move |tick| {
            engine_clone.on_tick(tick);
        })),
        on_order_update: Some(Box::new(|order| {
            info!("Order update: {:?} status={:?}", order.id, order.status);
        })),
        on_trade: Some(Box::new(|trade| {
            info!(
                "Trade: {} {} @ {} qty={}",
                trade.symbol,
                trade.side,
                from_price(trade.price),
                from_qty(trade.quantity)
            );
        })),
        on_error: Some(Box::new(|error| {
            error!("Exchange error: {}", error);
        })),
        on_connected: Some(Box::new(|| {
            info!("Connected to exchange");
        })),
        on_disconnected: Some(Box::new(|| {
            warn!("Disconnected from exchange");
        })),
    });

    // Connect to exchange
    info!("Connecting to exchange...");
    client.connect().await?;

    // Subscribe to market data
    info!("Subscribing to market data...");
    client.subscribe_ticker(&symbol).await?;
    client.subscribe_orderbook(&symbol, 20).await?;

    // Start engine
    info!("Starting trading engine...");
    engine_ref.start().await?;

    if !config.trading.paper_trading {
        engine_ref.enable_trading();
    }

    info!("System started. Press Ctrl+C to stop.");

    // Wait for shutdown signal
    signal::ctrl_c().await?;

    // Shutdown
    info!("Shutting down...");
    engine_ref.disable_trading();
    engine_ref.stop().await;
    client.disconnect().await?;

    let stats = engine_ref.stats();
    info!("Final Statistics:");
    info!("  Ticks processed: {}", stats.ticks_processed);
    info!("  Orders sent: {}", stats.orders_sent);
    info!("  Trades executed: {}", stats.trades_executed);

    info!("Shutdown complete.");
    Ok(())
}
