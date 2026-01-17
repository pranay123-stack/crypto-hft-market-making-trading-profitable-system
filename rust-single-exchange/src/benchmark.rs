//! Benchmark executable for performance testing

use hft::core::types::*;
use hft::orderbook::OrderBook;
use hft::strategy::{BasicMarketMaker, MarketMaker, MarketMakerParams, Signal};
use std::time::Instant;

fn main() {
    println!("HFT Performance Benchmarks\n");
    println!("==========================\n");

    bench_orderbook_updates();
    bench_orderbook_queries();
    bench_strategy_compute();
    bench_type_conversions();
}

fn bench_orderbook_updates() {
    println!("Order Book Updates:");
    println!("-------------------");

    let mut book = OrderBook::new(Symbol::new("BTCUSDT"));
    let iterations = 1_000_000;

    // Benchmark bid updates
    let start = Instant::now();
    for i in 0..iterations {
        let price = to_price(50000.0 + (i % 100) as f64);
        let qty = to_qty(1.0 + (i % 10) as f64);
        book.update_bid(price, qty);
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!(
        "  Bid updates: {:.2}M ops/sec ({:?} for {} ops)",
        ops_per_sec / 1_000_000.0,
        elapsed,
        iterations
    );

    // Benchmark ask updates
    let start = Instant::now();
    for i in 0..iterations {
        let price = to_price(50001.0 + (i % 100) as f64);
        let qty = to_qty(1.0 + (i % 10) as f64);
        book.update_ask(price, qty);
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!(
        "  Ask updates: {:.2}M ops/sec ({:?} for {} ops)",
        ops_per_sec / 1_000_000.0,
        elapsed,
        iterations
    );

    println!();
}

fn bench_orderbook_queries() {
    println!("Order Book Queries:");
    println!("-------------------");

    let mut book = OrderBook::new(Symbol::new("BTCUSDT"));

    // Populate book
    for i in 0..100 {
        book.update_bid(to_price(50000.0 - i as f64), to_qty(1.0));
        book.update_ask(to_price(50001.0 + i as f64), to_qty(1.0));
    }

    let iterations = 10_000_000;

    // Benchmark best bid/ask
    let start = Instant::now();
    for _ in 0..iterations {
        let _ = book.best_bid();
        let _ = book.best_ask();
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!(
        "  Best bid/ask: {:.2}M ops/sec",
        ops_per_sec / 1_000_000.0
    );

    // Benchmark mid price
    let start = Instant::now();
    for _ in 0..iterations {
        let _ = book.mid_price();
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!("  Mid price: {:.2}M ops/sec", ops_per_sec / 1_000_000.0);

    // Benchmark spread
    let start = Instant::now();
    for _ in 0..iterations {
        let _ = book.spread();
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!("  Spread: {:.2}M ops/sec", ops_per_sec / 1_000_000.0);

    // Benchmark imbalance
    let iterations = 1_000_000;
    let start = Instant::now();
    for _ in 0..iterations {
        let _ = book.imbalance(5);
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!("  Imbalance(5): {:.2}M ops/sec", ops_per_sec / 1_000_000.0);

    // Benchmark VWAP
    let start = Instant::now();
    for _ in 0..iterations {
        let _ = book.vwap_ask(to_qty(10.0));
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!("  VWAP: {:.2}M ops/sec", ops_per_sec / 1_000_000.0);

    println!();
}

fn bench_strategy_compute() {
    println!("Strategy Computation:");
    println!("---------------------");

    let params = MarketMakerParams::default();
    let mut strategy = BasicMarketMaker::new(params);
    strategy.set_enabled(true);

    let mut book = OrderBook::new(Symbol::new("BTCUSDT"));
    book.update_bid(to_price(50000.0), to_qty(10.0));
    book.update_ask(to_price(50001.0), to_qty(10.0));

    let signal = Signal::default();
    let iterations = 1_000_000;

    let start = Instant::now();
    for _ in 0..iterations {
        let _ = strategy.compute_quotes(&book, 0, &signal);
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!(
        "  Quote computation: {:.2}M ops/sec ({:.0} ns/op)",
        ops_per_sec / 1_000_000.0,
        elapsed.as_nanos() as f64 / iterations as f64
    );

    println!();
}

fn bench_type_conversions() {
    println!("Type Conversions:");
    println!("-----------------");

    let iterations = 100_000_000;

    // Benchmark to_price
    let start = Instant::now();
    for i in 0..iterations {
        let _ = to_price(50000.0 + i as f64 * 0.00000001);
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!("  to_price: {:.2}M ops/sec", ops_per_sec / 1_000_000.0);

    // Benchmark from_price
    let start = Instant::now();
    for i in 0..iterations {
        let _ = from_price(5000000000000 + i as i64);
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!("  from_price: {:.2}M ops/sec", ops_per_sec / 1_000_000.0);

    // Benchmark now_nanos
    let iterations = 10_000_000;
    let start = Instant::now();
    for _ in 0..iterations {
        let _ = now_nanos();
    }
    let elapsed = start.elapsed();
    let ops_per_sec = iterations as f64 / elapsed.as_secs_f64();
    println!("  now_nanos: {:.2}M ops/sec", ops_per_sec / 1_000_000.0);

    println!();
}
