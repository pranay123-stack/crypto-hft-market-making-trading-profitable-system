# Python Multi-Exchange HFT Market Making Bot

Multi-exchange market making and arbitrage system in Python.

## Features

- **Multi-Exchange Support**: Binance, Bybit, OKX
- **Consolidated Order Book**: Unified view across exchanges
- **NBBO Tracking**: National Best Bid/Offer
- **Cross-Exchange Arbitrage**: Detect and execute opportunities
- **Smart Hedging**: Instant hedging on alternate exchanges
- **Risk Management**: Per-exchange and total position limits

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

## Installation

```bash
# Install dependencies
pip install -e .

# Or with dev dependencies
pip install -e ".[dev]"
```

## Usage

```bash
# Run multi-exchange market maker
python -m src.main

# Run arbitrage bot only
python -m src.arbitrage_main
```

## Configuration

Set environment variables for API credentials:

```bash
export BINANCE_API_KEY="your_key"
export BINANCE_API_SECRET="your_secret"
export BYBIT_API_KEY="your_key"
export BYBIT_API_SECRET="your_secret"
export OKX_API_KEY="your_key"
export OKX_API_SECRET="your_secret"
export OKX_PASSPHRASE="your_passphrase"
```

## Project Structure

```
python-multi-exchange/
├── pyproject.toml
├── README.md
└── src/
    ├── __init__.py
    ├── main.py                  # Market maker entry
    ├── arbitrage_main.py        # Arbitrage entry
    ├── core/
    │   ├── __init__.py
    │   └── types.py             # Multi-exchange types
    ├── exchange/
    │   ├── __init__.py
    │   ├── base.py              # Exchange interface
    │   ├── manager.py           # Exchange manager
    │   ├── binance.py
    │   ├── bybit.py
    │   └── okx.py
    ├── orderbook/
    │   ├── __init__.py
    │   └── consolidated.py      # Consolidated book + NBBO
    ├── strategy/
    │   ├── __init__.py
    │   └── cross_exchange_mm.py # Cross-exchange MM
    ├── arbitrage/
    │   ├── __init__.py
    │   └── detector.py          # Arb detection/execution
    └── risk/
        ├── __init__.py
        └── manager.py           # Cross-exchange risk
```

## Key Components

### ConsolidatedBook
Aggregates order books from all exchanges and calculates NBBO.

### ArbitrageDetector
Monitors for cross-exchange price discrepancies exceeding minimum profit threshold.

### CrossExchangeMM
Market making strategy that quotes across exchanges with inventory-aware pricing.

### RiskManager
Tracks positions and P&L across all exchanges with kill switch functionality.

## Note

This is a Python implementation primarily suitable for:
- Research and backtesting
- Prototype development
- Lower-frequency strategies

For true HFT with sub-millisecond requirements, use the C++ or Rust implementations.

## License

MIT
