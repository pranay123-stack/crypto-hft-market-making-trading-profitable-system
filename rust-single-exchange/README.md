# Rust Crypto HFT Market Making Bot - Single Exchange

A high-performance market making system for cryptocurrency exchanges, built with Rust for maximum safety and performance.

## Features

- **Memory Safety**: Rust's ownership system prevents common bugs
- **Zero-Cost Abstractions**: High-level code with low-level performance
- **Async/Await**: Efficient concurrent I/O with Tokio
- **Lock-Free Data Structures**: SPSC queues for inter-thread communication
- **Type-Safe**: Fixed-point arithmetic for prices/quantities

## Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                      Trading Engine                             │
├────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐     │
│  │  Tick       │  │  Strategy   │  │  Order              │     │
│  │  Handler    │  │  Task       │  │  Handler            │     │
│  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘     │
│         │                │                     │                │
│         ▼                ▼                     ▼                │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │            Crossbeam Channels (Lock-Free)               │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│  ┌───────────────────────────┴───────────────────────────┐     │
│  │                    Core Components                     │     │
│  │  ┌─────────┐  ┌─────────────┐  ┌───────────────────┐  │     │
│  │  │OrderBook│  │ Risk Manager│  │  Memory Pool      │  │     │
│  │  │ (BTree) │  │             │  │                   │  │     │
│  │  └─────────┘  └─────────────┘  └───────────────────┘  │     │
│  └───────────────────────────────────────────────────────┘     │
│                              │                                  │
│  ┌───────────────────────────┴───────────────────────────┐     │
│  │              Binance Client (Async)                    │     │
│  │  ┌────────────────┐  ┌─────────────────────────────┐  │     │
│  │  │ WebSocket      │  │      REST (reqwest)         │  │     │
│  │  │ (tungstenite)  │  │                             │  │     │
│  │  └────────────────┘  └─────────────────────────────┘  │     │
│  └───────────────────────────────────────────────────────┘     │
└────────────────────────────────────────────────────────────────┘
```

## Requirements

- Rust 1.75+ (2021 edition)
- OpenSSL development libraries

## Building

```bash
# Debug build
cargo build

# Release build (optimized)
cargo build --release

# Run tests
cargo test

# Run benchmarks
cargo bench
```

## Usage

```bash
# Run with default config
cargo run --release

# Run with custom config
cargo run --release -- -c config/custom.json

# Run in testnet mode
cargo run --release -- --testnet

# Command line options
cargo run --release -- --help
```

## Configuration

Edit `config/config.json` or set environment variables:

```bash
export API_KEY="your_key"
export API_SECRET="your_secret"
export TRADING_SYMBOL="BTCUSDT"
export LOG_LEVEL="DEBUG"
```

## Project Structure

```
rust-single-exchange/
├── Cargo.toml
├── config/
│   └── config.json
├── src/
│   ├── lib.rs              # Library root
│   ├── main.rs             # Main executable
│   ├── benchmark.rs        # Performance benchmarks
│   ├── core/
│   │   ├── mod.rs
│   │   ├── types.rs        # Core types (Price, Qty, Order)
│   │   ├── engine.rs       # Trading engine
│   │   ├── queue.rs        # Lock-free queues
│   │   └── memory.rs       # Memory pools
│   ├── orderbook/
│   │   ├── mod.rs
│   │   └── book.rs         # Order book implementation
│   ├── strategy/
│   │   ├── mod.rs
│   │   └── market_maker.rs # MM strategies
│   ├── risk/
│   │   └── mod.rs          # Risk management
│   ├── exchange/
│   │   ├── mod.rs
│   │   └── binance.rs      # Binance client
│   └── utils/
│       ├── mod.rs
│       ├── config.rs       # Configuration
│       └── logger.rs       # Logging
├── tests/
└── benches/
```

## Performance

Typical benchmark results (Release mode, AMD Ryzen 9):

```
Order Book Updates:
  Bid updates: 15.2M ops/sec
  Ask updates: 14.8M ops/sec

Order Book Queries:
  Best bid/ask: 180.5M ops/sec
  Mid price: 95.2M ops/sec
  Spread: 92.1M ops/sec
  Imbalance(5): 8.5M ops/sec
  VWAP: 2.1M ops/sec

Strategy Computation:
  Quote computation: 12.5M ops/sec (80 ns/op)
```

## Key Design Decisions

### Fixed-Point Arithmetic
All prices and quantities use `i64` with 8 decimal places to avoid floating-point precision issues:
```rust
pub type Price = i64;
pub const PRECISION: i64 = 100_000_000;
```

### Lock-Free Communication
SPSC queues for market data flow:
```rust
pub struct SpscQueue<T> {
    buffer: Box<[UnsafeCell<Option<T>>]>,
    head: AtomicUsize,
    tail: AtomicUsize,
}
```

### BTreeMap for Order Book
Sorted map for efficient best bid/ask access:
```rust
bids: BTreeMap<Reverse<Price>, PriceLevel>  // Descending
asks: BTreeMap<Price, PriceLevel>           // Ascending
```

## Extending

### Adding a New Strategy

```rust
pub struct MyStrategy {
    params: MarketMakerParams,
}

impl MarketMaker for MyStrategy {
    fn compute_quotes(&mut self, book: &OrderBook, position: Quantity, signal: &Signal) -> QuoteDecision {
        // Your logic here
    }
    // ... implement other trait methods
}
```

### Adding a New Exchange

Implement the `ExchangeClient` trait:
```rust
#[async_trait]
impl ExchangeClient for MyExchangeClient {
    async fn connect(&mut self) -> Result<(), ExchangeError>;
    async fn send_order(&self, request: OrderRequest) -> Result<OrderResponse, ExchangeError>;
    // ... other methods
}
```

## License

MIT License

## Disclaimer

This software is for educational purposes only. Trading cryptocurrencies involves substantial risk. Use at your own risk.
