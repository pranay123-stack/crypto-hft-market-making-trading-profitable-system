//! Standalone arbitrage bot

use hft_multi::prelude::*;
use hft_multi::orderbook::ConsolidatedBook;
use hft_multi::arbitrage::{ArbitrageDetector, ArbitrageConfig, ArbitrageExecutor};
use std::sync::Arc;
use tokio::signal;
use tracing::info;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    println!("Arbitrage Bot - Multi Exchange");
    println!("==============================\n");

    tracing_subscriber::fmt().with_env_filter("info").init();

    let symbol = Symbol::new("BTCUSDT");
    let book = Arc::new(ConsolidatedBook::new(symbol.clone()));

    let config = ArbitrageConfig {
        min_profit_bps: 3.0,
        ..Default::default()
    };
    let detector = ArbitrageDetector::new(config);
    let executor = ArbitrageExecutor::new();

    info!("Monitoring {} for arbitrage opportunities", symbol);

    // Simulate data
    let book_clone = book.clone();
    tokio::spawn(async move {
        let mut i = 0u64;
        loop {
            let base = to_price(50000.0);
            // Occasionally create arbitrage opportunity
            let arb_spread = if i % 20 == 0 { to_price(5.0) } else { to_price(0.0) };

            book_clone.update(ExchangeId::Binance, base - to_price(1.0), to_qty(1.0), base + to_price(1.0), to_qty(1.0));
            book_clone.update(ExchangeId::Bybit, base - to_price(1.0) - arb_spread, to_qty(0.5), base + to_price(1.0) + arb_spread, to_qty(0.5));

            i += 1;
            tokio::time::sleep(tokio::time::Duration::from_millis(50)).await;
        }
    });

    loop {
        tokio::select! {
            _ = signal::ctrl_c() => break,
            _ = tokio::time::sleep(tokio::time::Duration::from_millis(10)) => {
                if let Some(opp) = detector.detect(&book) {
                    info!("ARB: {} -> {}, profit={:.2} bps", opp.buy_exchange, opp.sell_exchange, opp.profit_bps);
                    if let Ok(profit) = executor.execute(&opp).await {
                        info!("Executed: profit=${:.4}", profit);
                    }
                }
            }
        }
    }

    info!("Total opportunities: {}", detector.opportunities_found());
    Ok(())
}
