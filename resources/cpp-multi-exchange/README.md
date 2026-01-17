# C++ Crypto HFT Market Making Bot - Multi Exchange

A high-frequency trading market making system supporting multiple cryptocurrency exchanges simultaneously, with cross-exchange arbitrage capabilities.

## Architecture

```
┌────────────────────────────────────────────────────────────────────────────┐
│                        Multi-Exchange Trading Engine                        │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                      Exchange Manager                                │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐            │   │
│  │  │ Binance  │  │  Bybit   │  │   OKX    │  │  Kraken  │   ...      │   │
│  │  │ Connector│  │ Connector│  │ Connector│  │ Connector│            │   │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘            │   │
│  └───────┼─────────────┼─────────────┼─────────────┼──────────────────┘   │
│          │             │             │             │                       │
│          ▼             ▼             ▼             ▼                       │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    Consolidated Order Book                           │   │
│  │         (Aggregated view across all exchanges + NBBO)               │   │
│  └───────────────────────────────┬─────────────────────────────────────┘   │
│                                  │                                         │
│          ┌───────────────────────┼───────────────────────┐                │
│          │                       │                       │                │
│          ▼                       ▼                       ▼                │
│  ┌───────────────┐      ┌───────────────┐      ┌───────────────┐         │
│  │Cross-Exchange │      │   Arbitrage   │      │   Smart Order │         │
│  │ Market Maker  │      │   Detector    │      │    Router     │         │
│  └───────────────┘      └───────────────┘      └───────────────┘         │
│          │                       │                       │                │
│          └───────────────────────┼───────────────────────┘                │
│                                  ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                      Risk Manager (Cross-Exchange)                   │   │
│  │    Position Limits | P&L Tracking | Kill Switch | Rate Limits       │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────────┘
```

## Features

### Multi-Exchange Support
- **Binance** - Spot and Futures
- **Bybit** - Spot and Perpetuals
- **OKX** - Spot, Futures, and Swaps
- **Extensible** - Easy to add new exchanges

### Cross-Exchange Capabilities
- **Consolidated Order Book** - Unified view of liquidity
- **NBBO Tracking** - National Best Bid/Offer across exchanges
- **Smart Order Routing** - Route to best price/liquidity
- **Cross-Exchange Arbitrage** - Detect and execute arb opportunities

### Market Making
- **Multi-Exchange Quoting** - Quote on multiple venues
- **Inventory Balancing** - Distribute position across exchanges
- **Instant Hedging** - Hedge fills on other exchanges
- **Latency-Optimized** - Prefer faster exchanges for hedging

### Risk Management
- Per-exchange and total position limits
- Cross-exchange P&L tracking
- Coordinated kill switch
- Exchange health monitoring

## Building

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
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

Edit `config/config.json` for strategy parameters.

## Usage

```bash
# Run multi-exchange market maker
./multi_exchange_mm -c config/config.json

# Run arbitrage bot only
./arbitrage_bot -c config/config.json

# Options
./multi_exchange_mm --help
```

## Project Structure

```
cpp-multi-exchange/
├── include/
│   ├── core/
│   │   └── types.hpp              # Multi-exchange types
│   ├── exchange/
│   │   └── exchange_manager.hpp   # Exchange management
│   ├── orderbook/
│   │   └── consolidated_book.hpp  # Aggregated order book
│   ├── strategy/
│   │   └── cross_exchange_mm.hpp  # Cross-exchange MM
│   └── arbitrage/
│       └── arbitrage_detector.hpp # Arb detection
├── src/
│   ├── main.cpp                   # Market maker entry
│   └── arbitrage_main.cpp         # Arbitrage entry
├── config/
│   └── config.json
└── tests/
```

## Arbitrage Detection

The system detects several types of arbitrage:

### Cross-Exchange Arbitrage
Buy on exchange A, sell on exchange B when:
```
sell_price_B - buy_price_A > min_profit + fees + slippage
```

### Triangular Arbitrage
Within single exchange: A → B → C → A
```
(1/price_AB) * price_BC * price_CA > 1 + fees
```

## Smart Order Routing

Strategies:
- **BEST_PRICE** - Route to best bid/ask
- **LOWEST_LATENCY** - Route to fastest exchange
- **SPLIT_ORDER** - Distribute across exchanges
- **PRIORITY** - Use configured exchange priority

## Performance Considerations

1. **Latency Tracking** - Monitor per-exchange latency
2. **Parallel Connections** - Concurrent WebSocket streams
3. **Lock-Free Updates** - Minimize contention
4. **Exchange Health** - Automatic failover on issues

## License

MIT License

## Disclaimer

This software is for educational purposes only. Trading involves substantial risk. Use at your own risk.
