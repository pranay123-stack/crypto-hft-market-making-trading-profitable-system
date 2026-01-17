# C++ Crypto HFT Market Making Bot - Single Exchange

A high-frequency trading market making system for cryptocurrency exchanges, built with C++20 for maximum performance.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      Trading Engine                              │
├─────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │ Market Data  │  │  Strategy    │  │  Order Management    │  │
│  │   Thread     │  │   Thread     │  │      Thread          │  │
│  └──────┬───────┘  └──────┬───────┘  └──────────┬───────────┘  │
│         │                 │                      │              │
│         ▼                 ▼                      ▼              │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │              Lock-Free Message Queues                    │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│  ┌───────────────────────────┴───────────────────────────┐     │
│  │                    Core Components                     │     │
│  │  ┌─────────┐  ┌─────────────┐  ┌───────────────────┐  │     │
│  │  │OrderBook│  │ Risk Manager│  │ Position Manager  │  │     │
│  │  └─────────┘  └─────────────┘  └───────────────────┘  │     │
│  └───────────────────────────────────────────────────────┘     │
│                              │                                  │
│  ┌───────────────────────────┴───────────────────────────┐     │
│  │                 Exchange Connector                     │     │
│  │  ┌────────────────┐  ┌─────────────────────────────┐  │     │
│  │  │ WebSocket (MD) │  │      REST (Orders)          │  │     │
│  │  └────────────────┘  └─────────────────────────────┘  │     │
│  └───────────────────────────────────────────────────────┘     │
└─────────────────────────────────────────────────────────────────┘
```

## Features

- **Ultra-Low Latency**: Lock-free queues, memory pools, cache-optimized data structures
- **Market Making Strategies**: Basic MM, Inventory-Adjusted MM, Avellaneda-Stoikov
- **Risk Management**: Real-time position limits, loss limits, rate limiting, kill switch
- **Exchange Support**: Binance Spot/Futures (extensible to other exchanges)
- **Production Ready**: Comprehensive logging, metrics, configuration management

## Requirements

- C++20 compatible compiler (GCC 10+, Clang 12+)
- CMake 3.20+
- OpenSSL
- libcurl
- Boost 1.75+
- Optional: liburing (for io_uring support on Linux)

## Building

```bash
# Create build directory
mkdir build && cd build

# Configure (Release mode)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build
make -j$(nproc)

# Run tests
./unit_tests

# Run benchmark
./benchmark
```

## Configuration

Edit `config/config.json`:

```json
{
  "exchange": {
    "name": "binance",
    "api_key": "YOUR_API_KEY",
    "api_secret": "YOUR_API_SECRET"
  },
  "trading": {
    "symbol": "BTCUSDT",
    "paper_trading": true
  },
  "strategy": {
    "min_spread_bps": 5.0,
    "max_spread_bps": 50.0,
    "target_spread_bps": 10.0
  }
}
```

## Usage

```bash
# Run with default config
./market_maker

# Run with custom config
./market_maker -c /path/to/config.json

# Run in testnet mode
./market_maker -t -s BTCUSDT

# Command line options
./market_maker --help
```

## Project Structure

```
cpp-single-exchange/
├── CMakeLists.txt
├── config/
│   └── config.json
├── include/
│   ├── core/
│   │   ├── types.hpp          # Fundamental types (Order, Price, etc.)
│   │   ├── engine.hpp         # Trading engine
│   │   ├── lock_free_queue.hpp
│   │   └── memory_pool.hpp
│   ├── exchange/
│   │   ├── exchange_client.hpp
│   │   └── binance_client.hpp
│   ├── orderbook/
│   │   └── orderbook.hpp
│   ├── strategy/
│   │   └── market_maker.hpp
│   ├── risk/
│   │   └── risk_manager.hpp
│   └── utils/
│       ├── config.hpp
│       └── logger.hpp
├── src/
│   ├── main.cpp
│   ├── core/
│   ├── exchange/
│   ├── orderbook/
│   ├── strategy/
│   └── risk/
└── tests/
```

## Performance Optimizations

1. **Lock-Free Data Structures**: SPSC/MPMC queues for inter-thread communication
2. **Memory Pools**: Pre-allocated memory for orders to avoid heap allocations
3. **Cache-Line Alignment**: Critical structs aligned to 64 bytes
4. **CPU Affinity**: Pin threads to specific cores (optional)
5. **Compile-Time Optimizations**: `-O3 -march=native -flto`

## Market Making Strategy

The default strategy implements a simple market making approach:

1. Calculate fair value from mid-price
2. Set bid/ask spread based on volatility and inventory
3. Skew quotes based on current inventory
4. Manage position limits and risk

### Avellaneda-Stoikov Model

Also includes the Avellaneda-Stoikov optimal market making strategy:
- Calculates reservation price based on inventory risk
- Derives optimal spread from arrival rate and volatility
- Time-aware adjustments for end-of-period inventory

## Risk Management

- **Position Limits**: Maximum quantity and value per symbol
- **Order Limits**: Size, value, and rate limits
- **Loss Limits**: Per-trade, daily, and drawdown limits
- **Kill Switch**: Automatic trading halt on excessive errors/losses
- **Price Deviation**: Reject orders too far from mid-price

## Extending

### Adding a New Exchange

1. Implement `ExchangeClient` interface in `include/exchange/`
2. Add exchange-specific message parsing
3. Register in `ExchangeClientFactory`

### Adding a New Strategy

1. Inherit from `MarketMaker` class
2. Override `compute_quotes()` method
3. Implement custom fair value and spread calculations

## License

MIT License

## Disclaimer

This software is for educational purposes only. Trading cryptocurrencies involves substantial risk of loss. Use at your own risk.
