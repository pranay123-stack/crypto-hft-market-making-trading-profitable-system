/**
 * Strategy Benchmarks
 * Measures signal generation latency for all strategies
 */

#include <benchmark/benchmark.h>
#include <random>

#include "core/types.hpp"
#include "strategies/hft/latency_arbitrage.hpp"
#include "strategies/hft/triangular_arbitrage.hpp"
#include "strategies/hft/statistical_arbitrage.hpp"
#include "strategies/hft/momentum_ignition.hpp"
#include "strategies/hft/order_flow_imbalance.hpp"
#include "strategies/market_making/basic_market_maker.hpp"
#include "strategies/market_making/adaptive_market_maker.hpp"
#include "strategies/market_making/inventory_market_maker.hpp"
#include "strategies/market_making/grid_market_maker.hpp"
#include "strategies/market_making/avellaneda_stoikov.hpp"

using namespace hft;
using namespace hft::strategies;

// ============================================================================
// Helper Functions
// ============================================================================

MarketData generate_orderbook(double base_price, std::mt19937& rng) {
    MarketData data;
    data.symbol = "BTC-USDT";
    data.exchange = Exchange::Binance;
    data.timestamp = core::Timestamp::now();

    std::uniform_real_distribution<double> spread_dist(0.01, 0.05);
    std::uniform_real_distribution<double> size_dist(0.1, 10.0);

    double spread = base_price * spread_dist(rng) / 100;

    // Generate 10 levels each side
    for (int i = 0; i < 10; ++i) {
        data.bids.push_back({
            base_price - spread * (i + 1),
            size_dist(rng)
        });
        data.asks.push_back({
            base_price + spread * (i + 1),
            size_dist(rng)
        });
    }

    data.mid_price = base_price;
    return data;
}

// ============================================================================
// HFT Strategy Benchmarks
// ============================================================================

static void BM_LatencyArbitrage(benchmark::State& state) {
    StrategyParams params;
    params.set("min_spread_bps", 5.0);
    params.set("max_position", 10.0);
    params.set("timeout_ms", 100);

    hft::LatencyArbitrage strategy(params);

    std::mt19937 rng(42);
    double price = 50000.0;

    for (auto _ : state) {
        // Simulate price updates from two exchanges
        MarketData data1 = generate_orderbook(price, rng);
        data1.exchange = Exchange::Binance;

        MarketData data2 = generate_orderbook(price + 10, rng);
        data2.exchange = Exchange::Bybit;

        strategy.on_market_data(data1);
        strategy.on_market_data(data2);

        auto signals = strategy.get_signals();
        benchmark::DoNotOptimize(signals);

        price += (rng() % 21 - 10);  // Random walk
    }

    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_LatencyArbitrage);

static void BM_TriangularArbitrage(benchmark::State& state) {
    StrategyParams params;
    params.set("min_profit_bps", 3.0);
    params.set("max_position", 1.0);

    hft::TriangularArbitrage strategy(params);

    std::mt19937 rng(42);

    for (auto _ : state) {
        MarketData btc_usdt = generate_orderbook(50000, rng);
        btc_usdt.symbol = "BTC-USDT";

        MarketData eth_usdt = generate_orderbook(2500, rng);
        eth_usdt.symbol = "ETH-USDT";

        MarketData eth_btc = generate_orderbook(0.05, rng);
        eth_btc.symbol = "ETH-BTC";

        strategy.on_market_data(btc_usdt);
        strategy.on_market_data(eth_usdt);
        strategy.on_market_data(eth_btc);

        auto signals = strategy.get_signals();
        benchmark::DoNotOptimize(signals);
    }

    state.SetItemsProcessed(state.iterations() * 3);
}
BENCHMARK(BM_TriangularArbitrage);

static void BM_StatisticalArbitrage(benchmark::State& state) {
    StrategyParams params;
    params.set("lookback_period", 100);
    params.set("entry_zscore", 2.0);
    params.set("exit_zscore", 0.5);

    hft::StatisticalArbitrage strategy(params);

    std::mt19937 rng(42);
    double btc_price = 50000;
    double eth_price = 2500;

    for (auto _ : state) {
        MarketData btc = generate_orderbook(btc_price, rng);
        btc.symbol = "BTC-USDT";

        MarketData eth = generate_orderbook(eth_price, rng);
        eth.symbol = "ETH-USDT";

        strategy.on_market_data(btc);
        strategy.on_market_data(eth);

        auto signals = strategy.get_signals();
        benchmark::DoNotOptimize(signals);

        // Correlated random walk
        double move = (rng() % 21 - 10);
        btc_price += move;
        eth_price += move * 0.05;
    }

    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_StatisticalArbitrage);

static void BM_MomentumIgnition(benchmark::State& state) {
    StrategyParams params;
    params.set("momentum_window", 10);
    params.set("volume_threshold", 100);
    params.set("min_momentum", 0.001);

    hft::MomentumIgnition strategy(params);

    std::mt19937 rng(42);
    double price = 50000;

    for (auto _ : state) {
        MarketData data = generate_orderbook(price, rng);
        strategy.on_market_data(data);

        // Simulate trade
        Trade trade;
        trade.price = price;
        trade.quantity = (rng() % 10) + 0.1;
        trade.side = (rng() % 2) ? Side::Buy : Side::Sell;
        strategy.on_trade(trade);

        auto signals = strategy.get_signals();
        benchmark::DoNotOptimize(signals);

        price += (rng() % 11 - 5);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MomentumIgnition);

static void BM_OrderFlowImbalance(benchmark::State& state) {
    StrategyParams params;
    params.set("imbalance_threshold", 0.3);
    params.set("window_size", 50);
    params.set("decay_factor", 0.95);

    hft::OrderFlowImbalance strategy(params);

    std::mt19937 rng(42);
    double price = 50000;

    for (auto _ : state) {
        MarketData data = generate_orderbook(price, rng);
        strategy.on_market_data(data);

        auto signals = strategy.get_signals();
        benchmark::DoNotOptimize(signals);

        price += (rng() % 11 - 5);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderFlowImbalance);

// ============================================================================
// Market Making Strategy Benchmarks
// ============================================================================

static void BM_BasicMarketMaker(benchmark::State& state) {
    StrategyParams params;
    params.set("spread_bps", 10.0);
    params.set("order_size", 0.1);
    params.set("num_levels", 3);

    mm::BasicMarketMaker strategy(params);

    std::mt19937 rng(42);
    double price = 50000;

    for (auto _ : state) {
        MarketData data = generate_orderbook(price, rng);
        strategy.on_market_data(data);

        auto signals = strategy.get_signals();
        benchmark::DoNotOptimize(signals);

        price += (rng() % 11 - 5);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BasicMarketMaker);

static void BM_AdaptiveMarketMaker(benchmark::State& state) {
    StrategyParams params;
    params.set("base_spread_bps", 10.0);
    params.set("volatility_multiplier", 2.0);
    params.set("order_size", 0.1);

    mm::AdaptiveMarketMaker strategy(params);

    std::mt19937 rng(42);
    double price = 50000;

    for (auto _ : state) {
        MarketData data = generate_orderbook(price, rng);
        strategy.on_market_data(data);

        auto signals = strategy.get_signals();
        benchmark::DoNotOptimize(signals);

        price += (rng() % 21 - 10);  // More volatile
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AdaptiveMarketMaker);

static void BM_InventoryMarketMaker(benchmark::State& state) {
    StrategyParams params;
    params.set("base_spread_bps", 10.0);
    params.set("inventory_skew_factor", 0.5);
    params.set("max_inventory", 10.0);

    mm::InventoryMarketMaker strategy(params);

    std::mt19937 rng(42);
    double price = 50000;
    double inventory = 0;

    for (auto _ : state) {
        strategy.set_inventory(inventory);

        MarketData data = generate_orderbook(price, rng);
        strategy.on_market_data(data);

        auto signals = strategy.get_signals();
        benchmark::DoNotOptimize(signals);

        price += (rng() % 11 - 5);
        inventory += (rng() % 3 - 1) * 0.1;  // Random inventory changes
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_InventoryMarketMaker);

static void BM_GridMarketMaker(benchmark::State& state) {
    StrategyParams params;
    params.set("grid_spacing_pct", 0.5);
    params.set("num_grids", state.range(0));
    params.set("order_size", 0.1);

    mm::GridMarketMaker strategy(params);

    std::mt19937 rng(42);
    double price = 50000;

    for (auto _ : state) {
        MarketData data = generate_orderbook(price, rng);
        strategy.on_market_data(data);

        auto signals = strategy.get_signals();
        benchmark::DoNotOptimize(signals);

        price += (rng() % 11 - 5);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GridMarketMaker)->Arg(5)->Arg(10)->Arg(20)->Arg(50);

static void BM_AvellanedaStoikov(benchmark::State& state) {
    StrategyParams params;
    params.set("gamma", 0.1);
    params.set("sigma", 0.02);
    params.set("T", 1.0);
    params.set("k", 1.5);
    params.set("max_inventory", 10);

    mm::AvellanedaStoikov strategy(params);

    std::mt19937 rng(42);
    double price = 50000;
    double inventory = 0;

    for (auto _ : state) {
        strategy.set_inventory(inventory);

        MarketData data = generate_orderbook(price, rng);
        strategy.on_market_data(data);

        auto signals = strategy.get_signals();
        benchmark::DoNotOptimize(signals);

        price += (rng() % 11 - 5);
        inventory += (rng() % 3 - 1) * 0.1;
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AvellanedaStoikov);

// ============================================================================
// Combined Strategy Benchmarks
// ============================================================================

static void BM_AllStrategiesParallel(benchmark::State& state) {
    // Initialize all strategies
    StrategyParams default_params;

    auto latency_arb = std::make_unique<hft::LatencyArbitrage>(default_params);
    auto triangular_arb = std::make_unique<hft::TriangularArbitrage>(default_params);
    auto stat_arb = std::make_unique<hft::StatisticalArbitrage>(default_params);
    auto momentum = std::make_unique<hft::MomentumIgnition>(default_params);
    auto order_flow = std::make_unique<hft::OrderFlowImbalance>(default_params);
    auto basic_mm = std::make_unique<mm::BasicMarketMaker>(default_params);
    auto adaptive_mm = std::make_unique<mm::AdaptiveMarketMaker>(default_params);
    auto inventory_mm = std::make_unique<mm::InventoryMarketMaker>(default_params);
    auto grid_mm = std::make_unique<mm::GridMarketMaker>(default_params);
    auto as_mm = std::make_unique<mm::AvellanedaStoikov>(default_params);

    std::mt19937 rng(42);
    double price = 50000;

    for (auto _ : state) {
        MarketData data = generate_orderbook(price, rng);

        // Process through all strategies
        latency_arb->on_market_data(data);
        triangular_arb->on_market_data(data);
        stat_arb->on_market_data(data);
        momentum->on_market_data(data);
        order_flow->on_market_data(data);
        basic_mm->on_market_data(data);
        adaptive_mm->on_market_data(data);
        inventory_mm->on_market_data(data);
        grid_mm->on_market_data(data);
        as_mm->on_market_data(data);

        // Get all signals
        auto s1 = latency_arb->get_signals();
        auto s2 = triangular_arb->get_signals();
        auto s3 = stat_arb->get_signals();
        auto s4 = momentum->get_signals();
        auto s5 = order_flow->get_signals();
        auto s6 = basic_mm->get_signals();
        auto s7 = adaptive_mm->get_signals();
        auto s8 = inventory_mm->get_signals();
        auto s9 = grid_mm->get_signals();
        auto s10 = as_mm->get_signals();

        benchmark::DoNotOptimize(s1);
        benchmark::DoNotOptimize(s10);

        price += (rng() % 11 - 5);
    }

    state.SetItemsProcessed(state.iterations() * 10);
}
BENCHMARK(BM_AllStrategiesParallel);

// ============================================================================
// Main
// ============================================================================

BENCHMARK_MAIN();
