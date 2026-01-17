//! Multi-Exchange HFT Market Making Bot

use hft_multi::prelude::*;
use hft_multi::exchange::{ExchangeManager, binance::BinanceClient, bybit::BybitClient, ExchangeClient};
use hft_multi::orderbook::ConsolidatedBook;
use hft_multi::strategy::{CrossExchangeMM, CrossExchangeMMParams, CrossExchangePosition};
use hft_multi::arbitrage::{ArbitrageDetector, ArbitrageConfig, ArbitrageExecutor};
use hft_multi::risk::{RiskManager, CrossExchangeRiskLimits};

use std::sync::Arc;
use tokio::signal;
use tracing::{info, warn, error};

fn print_banner() {
    println!(r#"
╔═══════════════════════════════════════════════════════════════════════╗
║        Crypto HFT Market Making System v1.0.0 - Multi Exchange        ║
║               High-Frequency Trading Bot (Rust Edition)               ║
╚═══════════════════════════════════════════════════════════════════════╝
"#);
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    print_banner();

    // Initialize logging
    tracing_subscriber::fmt()
        .with_env_filter("info")
        .init();

    info!("Starting Multi-Exchange HFT Bot");

    // Initialize exchange manager
    let exchange_manager = Arc::new(ExchangeManager::new());

    // Add exchange clients
    let binance = Arc::new(BinanceClient::new(true)) as Arc<dyn ExchangeClient>;
    let bybit = Arc::new(BybitClient::new(true)) as Arc<dyn ExchangeClient>;

    exchange_manager.add_client(binance);
    exchange_manager.add_client(bybit);

    info!("Configured {} exchanges", 2);

    // Initialize consolidated order book
    let symbol = Symbol::new("BTCUSDT");
    let consolidated_book = Arc::new(ConsolidatedBook::new(symbol.clone()));

    // Initialize cross-exchange market maker
    let mm_params = CrossExchangeMMParams {
        target_spread_bps: 15.0,
        max_position_per_exchange: to_qty(0.1),
        max_total_position: to_qty(0.2),
        default_order_size: to_qty(0.001),
        hedge_immediately: true,
        quote_exchanges: vec![ExchangeId::Binance, ExchangeId::Bybit],
        hedge_exchanges: vec![ExchangeId::Binance, ExchangeId::Bybit],
    };
    let mut market_maker = CrossExchangeMM::new(mm_params);

    // Initialize arbitrage detector
    let arb_config = ArbitrageConfig {
        min_profit_bps: 5.0,
        max_slippage_bps: 2.0,
        min_quantity: to_qty(0.001),
        max_quantity: to_qty(0.1),
        max_age_ns: 100_000_000,
    };
    let arb_detector = ArbitrageDetector::new(arb_config);
    let arb_executor = ArbitrageExecutor::new();

    // Initialize risk manager
    let risk_limits = CrossExchangeRiskLimits::default();
    let mut risk_manager = RiskManager::new(risk_limits);

    info!("All components initialized");
    info!("Monitoring {} across exchanges", symbol);

    // Enable market maker
    market_maker.enable();

    // Simulate market data updates (in production, would come from WebSocket)
    let book = consolidated_book.clone();
    tokio::spawn(async move {
        let mut counter = 0u64;
        loop {
            // Simulate price updates
            let base_price = to_price(50000.0);
            let noise = ((counter % 100) as i64 - 50) * to_price(0.1);

            // Binance quotes
            book.update(
                ExchangeId::Binance,
                base_price + noise - to_price(0.5),
                to_qty(1.0),
                base_price + noise + to_price(0.5),
                to_qty(1.0),
            );

            // Bybit quotes (slightly different)
            book.update(
                ExchangeId::Bybit,
                base_price + noise - to_price(0.6),
                to_qty(0.8),
                base_price + noise + to_price(0.4),
                to_qty(0.9),
            );

            counter += 1;
            tokio::time::sleep(tokio::time::Duration::from_millis(100)).await;
        }
    });

    // Main loop
    let mut position = CrossExchangePosition::default();
    let mut loop_count = 0u64;

    info!("Entering main loop. Press Ctrl+C to stop.");

    loop {
        tokio::select! {
            _ = signal::ctrl_c() => {
                info!("Received shutdown signal");
                break;
            }
            _ = tokio::time::sleep(tokio::time::Duration::from_millis(100)) => {
                loop_count += 1;

                // Check for arbitrage
                if let Some(opp) = arb_detector.detect(&consolidated_book) {
                    info!(
                        "Arbitrage opportunity: buy on {} @ {}, sell on {} @ {}, profit={:.2} bps",
                        opp.buy_exchange,
                        from_price(opp.buy_price),
                        opp.sell_exchange,
                        from_price(opp.sell_price),
                        opp.profit_bps
                    );

                    // Execute arbitrage
                    if let Ok(profit) = arb_executor.execute(&opp).await {
                        info!("Arbitrage executed, profit: ${:.4}", profit);
                        arb_detector.record_execution(true);
                    }
                }

                // Compute market making quotes
                if market_maker.is_enabled() && !risk_manager.is_kill_switch_active() {
                    let quotes = market_maker.compute_quotes(&consolidated_book, &position);

                    if quotes.should_quote {
                        for (exchange, bid, ask, bid_sz, ask_sz) in &quotes.quotes {
                            // Check risk before sending
                            if risk_manager.check_order(*exchange, Side::Buy, *bid_sz) {
                                // Would send order here
                            }
                        }
                    }
                }

                // Print periodic stats
                if loop_count % 50 == 0 {
                    let nbbo = consolidated_book.get_nbbo();
                    if nbbo.is_valid() {
                        info!(
                            "NBBO: bid={:.2} ({}) ask={:.2} ({}) spread={:.2} bps",
                            from_price(nbbo.best_bid),
                            nbbo.best_bid_exchange,
                            from_price(nbbo.best_ask),
                            nbbo.best_ask_exchange,
                            nbbo.spread_bps()
                        );
                    }

                    info!(
                        "Stats: arb_found={}, quotes={}, fills={}, pnl={:.2}",
                        arb_detector.opportunities_found(),
                        market_maker.quotes_sent(),
                        market_maker.fills(),
                        risk_manager.get_daily_pnl()
                    );
                }
            }
        }
    }

    // Shutdown
    info!("Shutting down...");
    market_maker.disable();

    info!("Final Statistics:");
    info!("  Arbitrage opportunities found: {}", arb_detector.opportunities_found());
    info!("  Arbitrage opportunities executed: {}", arb_detector.opportunities_executed());
    info!("  Market making quotes: {}", market_maker.quotes_sent());
    info!("  Market making fills: {}", market_maker.fills());
    info!("  Daily P&L: ${:.2}", risk_manager.get_daily_pnl());

    info!("Shutdown complete.");
    Ok(())
}
