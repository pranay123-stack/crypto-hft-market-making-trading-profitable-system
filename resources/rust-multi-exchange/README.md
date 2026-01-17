# Rust Multi-Exchange HFT Market Making Bot

Multi-exchange market making system with cross-exchange arbitrage, built in Rust.

## Features

- **Multi-Exchange Support**: Binance, Bybit, OKX (extensible)
- **Consolidated Order Book**: Unified view across exchanges
- **NBBO Tracking**: National Best Bid/Offer
- **Cross-Exchange Arbitrage**: Detect and execute opportunities
- **Smart Hedging**: Instant hedging on alternate exchanges
- **Risk Management**: Per-exchange and total position limits

## Building

```bash
cargo build --release
```

## Usage

```bash
# Run market maker
cargo run --release --bin multi-exchange-mm

# Run arbitrage bot only
cargo run --release --bin arbitrage-bot
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Exchange Manager                          │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐                     │
│  │ Binance │  │  Bybit  │  │   OKX   │                     │
│  └────┬────┘  └────┬────┘  └────┬────┘                     │
│       └────────────┼────────────┘                           │
│                    ▼                                         │
│          ┌─────────────────────┐                            │
│          │ Consolidated Book   │                            │
│          │     (NBBO)          │                            │
│          └──────────┬──────────┘                            │
│                     │                                        │
│     ┌───────────────┼───────────────┐                       │
│     ▼               ▼               ▼                       │
│ ┌────────┐    ┌──────────┐    ┌───────────┐                │
│ │Cross-Ex│    │ Arbitrage│    │   Risk    │                │
│ │  MM    │    │ Detector │    │  Manager  │                │
│ └────────┘    └──────────┘    └───────────┘                │
└─────────────────────────────────────────────────────────────┘
```

## Key Components

- `ConsolidatedBook`: Aggregates order books from all exchanges
- `NBBO`: Best bid/offer across all venues
- `CrossExchangeMM`: Market making across multiple exchanges
- `ArbitrageDetector`: Finds cross-exchange arbitrage
- `RiskManager`: Cross-exchange position and loss limits

## License

MIT
