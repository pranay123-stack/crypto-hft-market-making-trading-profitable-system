# Multi-Exchange Crypto HFT System with Level 2 Order Book Management

Institutional-grade, low-latency C++ high-frequency trading system with comprehensive Level 2 (L2) order book data handling across 10+ cryptocurrency exchanges.

## Key Features

### Multi-Exchange Support (10 Exchanges)
| Exchange | Spot | Futures | WebSocket | REST | L2 Depth |
|----------|------|---------|-----------|------|----------|
| Binance | Yes | Yes | Yes | Yes | 5-50 levels |
| Bybit | Yes | Yes | Yes | Yes | 25-200 levels |
| OKX | Yes | Yes | Yes | Yes | 5-400 levels |
| Kraken | Yes | Yes | Yes | Yes | 10-500 levels |
| Coinbase | Yes | - | Yes | Yes | 50 levels |
| KuCoin | Yes | Yes | Yes | Yes | 20-100 levels |
| Gate.io | Yes | Yes | Yes | Yes | 5-50 levels |
| Bitfinex | Yes | - | Yes | Yes | 25-250 levels |
| Deribit | - | Yes | Yes | Yes | 10-100 levels |
| HTX | Yes | Yes | Yes | Yes | 5-150 levels |

### Level 2 Order Book Capabilities
- **Real-time delta processing** with sequence validation
- **Snapshot initialization** with incremental sync
- **Gap detection** with automatic recovery
- **Staleness monitoring** (configurable threshold, default 5s)
- **O(log n) price level operations** using sorted maps
- **Lock-free atomics** for thread-safe concurrent access
- **Nanosecond-precision timestamps** for latency measurement

### Trading Modes
- **Live Trading** - Production deployment with real funds
- **Paper Trading** - Simulated trading with live market data
- **Backtesting** - Historical data replay with latency emulation

### HFT Strategies (5)
| Strategy | Description | Latency Target |
|----------|-------------|----------------|
| Latency Arbitrage | Cross-exchange price discrepancy exploitation | <100us |
| Triangular Arbitrage | Currency triangle inefficiencies | <200us |
| Statistical Arbitrage | Pairs trading with cointegration | <500us |
| Momentum Ignition | Order flow momentum detection | <100us |
| Order Flow Imbalance | Microstructure-based signals | <100us |

### Market Making Strategies (5)
| Strategy | Description | Use Case |
|----------|-------------|----------|
| Basic MM | Simple spread-based quoting | Low volatility |
| Adaptive MM | Volatility-adjusted spreads | Variable markets |
| Inventory MM | Inventory-aware skewing | Risk management |
| Grid MM | Multi-level grid orders | Range-bound markets |
| Avellaneda-Stoikov | Optimal MM theory implementation | Professional MM |

---

## Architecture

```
+------------------------------------------------------------------+
|                        Trading Engine                             |
+------------------------------------------------------------------+
|                                                                   |
|  +------------------+  +------------------+  +------------------+ |
|  |  HFT Strategies  |  |  MM Strategies   |  | Strategy Manager | |
|  |  (5 strategies)  |  |  (5 strategies)  |  |                  | |
|  +--------+---------+  +--------+---------+  +--------+---------+ |
|           |                     |                     |           |
|           +---------------------+---------------------+           |
|                                 |                                 |
|  +--------------------------------------------------------------+ |
|  |              Order Management System (OMS)                    | |
|  |  +----------------+  +----------------+  +------------------+ | |
|  |  | Order Manager  |  | Position Mgr   |  | Execution Engine | | |
|  |  +----------------+  +----------------+  +------------------+ | |
|  +--------------------------------------------------------------+ |
|                                 |                                 |
|  +--------------------------------------------------------------+ |
|  |                    Risk Management                            | |
|  |  +----------------+  +----------------+  +------------------+ | |
|  |  | Risk Manager   |  | PnL Tracker    |  | Circuit Breaker  | | |
|  |  +----------------+  +----------------+  +------------------+ | |
|  +--------------------------------------------------------------+ |
+------------------------------------------------------------------+
                                  |
    +-----------------------------+-----------------------------+
    |              Market Data & Order Book Layer               |
    |  +------------------------------------------------------+ |
    |  |           OrderBookManager (Central Hub)              | |
    |  |  - Real-time L2 depth management                      | |
    |  |  - Delta processing with sequence validation          | |
    |  |  - Gap detection & automatic snapshot recovery        | |
    |  |  - Staleness monitoring (background thread)           | |
    |  +------------------------------------------------------+ |
    +-----------------------------+-----------------------------+
                                  |
    +--------+--------+--------+--------+--------+--------+-----+
    |Binance |  Bybit |   OKX  | Kraken |Coinbase| KuCoin | ... |
    +--------+--------+--------+--------+--------+--------+-----+
```

---

## Level 2 Order Book Implementation

### Core Data Structures

```cpp
// Price level in orderbook (include/exchange/exchange_base.hpp)
struct PriceLevel {
    double price{0.0};
    double quantity{0.0};
    uint32_t order_count{0};  // Number of orders at this level
};

// Orderbook snapshot
struct OrderBook {
    std::string symbol;
    std::string exchange;
    uint64_t sequence{0};            // Exchange sequence number
    uint64_t timestamp{0};           // Exchange timestamp (ms)
    uint64_t local_timestamp{0};     // Local receive timestamp (ns)

    std::vector<PriceLevel> bids;    // Sorted by price descending
    std::vector<PriceLevel> asks;    // Sorted by price ascending

    // Quick accessors
    double bestBid() const;
    double bestAsk() const;
    double midPrice() const;
    double spread() const;
    double spreadBps() const;
};
```

### Order Book Manager (include/market_data/order_book_manager.hpp)

```cpp
// Book states for lifecycle management
enum class BookState : uint8_t {
    Uninitialized,  // No data received yet
    Initializing,   // Waiting for snapshot
    Ready,          // Normal operation
    Stale,          // No updates for too long
    GapDetected,    // Sequence gap, needs resync
    Error           // Error state
};

// Configuration
struct OrderBookManagerConfig {
    size_t max_depth{50};                       // Max price levels
    uint64_t staleness_threshold_us{5000000};   // 5 seconds
    uint64_t gap_recovery_timeout_us{10000000}; // 10 seconds
    bool enable_staleness_check{true};
    bool enable_statistics{true};
    size_t event_queue_size{65536};
};
```

### Key Features

1. **Sorted Price Levels** - O(log n) operations using `std::map`
   ```cpp
   std::map<double, PriceLevel, std::greater<double>> bids_;  // Descending
   std::map<double, PriceLevel, std::less<double>> asks_;     // Ascending
   ```

2. **Sequence Validation** - Detects gaps in update stream
   ```cpp
   bool applyDelta(const OrderBookDelta& delta) {
       uint64_t expected = last_update_id_.load() + 1;
       if (delta.first_update_id > expected) {
           state_.store(BookState::GapDetected);
           return false;  // Triggers snapshot recovery
       }
       // Apply updates...
   }
   ```

3. **Automatic Snapshot Recovery** - On gap detection
   ```cpp
   void OrderBookManager::requestSnapshot(const std::string& symbol,
                                          const std::string& exchange) {
       if (snapshot_request_callback_) {
           snapshot_request_callback_(symbol, exchange);
       }
   }
   ```

4. **Staleness Monitoring** - Background thread checks for stale data
   ```cpp
   void stalenessCheckLoop() {
       while (running_) {
           for (const auto& [key, book] : books_) {
               uint64_t age_ns = now - book->getLastUpdateTime();
               if (age_ns > threshold_ns) {
                   book->markStale();
                   requestSnapshot(symbol, exchange);
               }
           }
       }
   }
   ```

5. **Callbacks for Events**
   ```cpp
   using OnBookUpdateCallback = std::function<void(const OrderBook&)>;
   using OnGapDetectedCallback = std::function<void(const GapEvent&)>;
   using OnStateChangeCallback = std::function<void(symbol, exchange, old, new)>;
   using OnStalenessCallback = std::function<void(symbol, exchange, age_us)>;
   ```

---

## Project Structure

```
cpp-multi-exchange-orderbook-hft/
|-- CMakeLists.txt
|-- include/
|   |-- backtesting/
|   |   |-- backtest_engine.hpp
|   |   |-- data_feed.hpp
|   |   |-- performance_analyzer.hpp
|   |   +-- simulated_exchange.hpp
|   |-- config/
|   |   |-- config_manager.hpp
|   |   |-- exchange_config.hpp
|   |   +-- strategy_config.hpp
|   |-- core/
|   |   |-- lock_free_queue.hpp
|   |   |-- memory_pool.hpp
|   |   +-- timing.hpp
|   |-- exchange/
|   |   |-- exchange_base.hpp
|   |   |-- exchange_factory.hpp
|   |   |-- binance.hpp
|   |   |-- bybit.hpp
|   |   |-- okx.hpp
|   |   |-- kraken.hpp
|   |   |-- coinbase.hpp
|   |   |-- kucoin.hpp
|   |   |-- gateio.hpp
|   |   |-- bitfinex.hpp
|   |   |-- deribit.hpp
|   |   +-- htx.hpp
|   |-- market_data/
|   |   +-- order_book_manager.hpp    # L2 Order Book Management
|   |-- monitoring/
|   |   +-- metrics_collector.hpp
|   |-- network/
|   |   |-- rest_client.hpp
|   |   +-- websocket_client.hpp
|   |-- risk/
|   |   +-- risk_manager.hpp
|   +-- strategy/
|       |-- strategy_base.hpp
|       |-- hft/
|       +-- market_making/
|-- src/
|   |-- main.cpp
|   |-- backtesting/
|   |-- config/
|   |-- exchange/
|   |-- market_data/
|   |   +-- order_book_manager.cpp    # L2 Implementation
|   |-- monitoring/
|   |-- network/
|   |-- risk/
|   +-- strategy/
|-- config/
|   |-- exchanges.yaml
|   |-- strategies.yaml
|   |-- risk.yaml
|   +-- system.yaml
|-- tests/
|-- benchmarks/
+-- docs/
```

---

## Requirements

### Compiler & Build Tools
- C++20 compiler (GCC 11+, Clang 14+, MSVC 2022+)
- CMake 3.20+

### Dependencies
- Boost 1.75+ (asio, beast, system)
- OpenSSL 1.1+
- libcurl
- yaml-cpp
- nlohmann/json

### Optional
- Google Test (unit testing)
- Google Benchmark (performance benchmarks)

---

## Quick Start

### Build

```bash
# Clone repository
git clone https://github.com/pranay123-stack/crypto-hft-market-making-trading-profitable-system.git
cd crypto-hft-market-making-trading-profitable-system/cpp-multi-exchange-orderbook-hft

# Build
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Configuration

Edit `config/exchanges.yaml`:
```yaml
exchanges:
  binance:
    api_key: "your_api_key"
    api_secret: "your_api_secret"
    testnet: true
    orderbook_depth: 20

  bybit:
    api_key: "your_api_key"
    api_secret: "your_api_secret"
    testnet: true
```

### Run

```bash
# Paper trading mode
./crypto_hft --mode paper --config ../config/system.yaml

# Backtesting mode
./crypto_hft --mode backtest --data ../data/historical/

# Live trading (requires API keys)
./crypto_hft --mode live --config ../config/system.yaml
```

---

## Performance Targets

| Component | Latency Target |
|-----------|----------------|
| Market data parsing | <1us |
| Order book update | <5us |
| Signal generation | <50us |
| Order routing | <100us |
| End-to-end tick-to-trade | <500us |

### Optimizations Used
- Lock-free queues for inter-thread communication
- Memory pools for zero-allocation hot paths
- RDTSC timing for nanosecond precision
- Cache-line aligned data structures
- Kernel bypass ready (DPDK compatible)

---

## Testing

```bash
cd build

# Run unit tests
ctest --output-on-failure

# Run specific test suite
./tests/test_order_book_manager
./tests/test_exchange_binance

# Run benchmarks
./benchmarks/benchmark_core
./benchmarks/benchmark_order_book
```

---

## API Example

### Order Book Subscription

```cpp
#include "exchange/binance.hpp"
#include "market_data/order_book_manager.hpp"

int main() {
    using namespace hft;

    // Create exchange connection
    exchange::ExchangeConfig config;
    config.name = "binance";
    config.api_key = "your_key";
    config.api_secret = "your_secret";

    auto binance = std::make_shared<exchange::BinanceExchange>(config);

    // Create order book manager
    market_data::OrderBookManagerConfig ob_config;
    ob_config.max_depth = 50;
    ob_config.staleness_threshold_us = 5000000;  // 5 seconds

    market_data::OrderBookManager manager(ob_config);

    // Set callbacks
    manager.setOnBookUpdate([](const exchange::OrderBook& book) {
        std::cout << "Book update: " << book.symbol
                  << " bid=" << book.bestBid()
                  << " ask=" << book.bestAsk()
                  << " spread=" << book.spreadBps() << "bps\n";
    });

    manager.setOnGapDetected([](const market_data::GapEvent& gap) {
        std::cout << "Gap detected: expected=" << gap.expected_id
                  << " received=" << gap.received_id << "\n";
    });

    // Connect and subscribe
    binance->connect();
    binance->subscribeOrderBook("BTCUSDT", 20);

    // Run event loop
    manager.start();

    // ... trading logic ...

    manager.stop();
    binance->disconnect();

    return 0;
}
```

---

## License

Proprietary - All rights reserved.

---

## Author

**Pranay Gaurav**
HFT/MFT Quant Developer
[GitHub](https://github.com/pranay123-stack) | pranaygaurav4555@gmail.com
