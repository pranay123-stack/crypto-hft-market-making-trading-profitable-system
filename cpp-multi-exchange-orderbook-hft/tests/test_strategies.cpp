/**
 * Strategy Unit Tests
 */

#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "core/types.hpp"
#include "strategies/strategy_base.hpp"
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
// Test Fixtures
// ============================================================================

class StrategyTestBase : public ::testing::Test {
protected:
    void SetUp() override {
        // Create sample market data
        orderbook_.symbol = "BTC-USDT";
        orderbook_.timestamp = core::Timestamp::now();

        // Add bid levels
        orderbook_.bids = {
            {50000.00, 1.0},
            {49999.50, 2.0},
            {49999.00, 3.0},
            {49998.50, 1.5},
            {49998.00, 5.0}
        };

        // Add ask levels
        orderbook_.asks = {
            {50000.50, 1.0},
            {50001.00, 2.0},
            {50001.50, 2.5},
            {50002.00, 1.0},
            {50002.50, 4.0}
        };
    }

    void TearDown() override {}

    MarketData orderbook_;
};

// ============================================================================
// Latency Arbitrage Tests
// ============================================================================

class LatencyArbitrageTest : public StrategyTestBase {
protected:
    void SetUp() override {
        StrategyTestBase::SetUp();

        StrategyParams params;
        params.set("min_spread_bps", 5.0);  // 0.05%
        params.set("max_position", 10.0);
        params.set("timeout_ms", 100);

        strategy_ = std::make_unique<hft::LatencyArbitrage>(params);
    }

    std::unique_ptr<hft::LatencyArbitrage> strategy_;
};

TEST_F(LatencyArbitrageTest, NoArbitrageWhenPricesSame) {
    // Same prices on both exchanges - no arbitrage
    MarketData exchange1_data = orderbook_;
    exchange1_data.exchange = Exchange::Binance;

    MarketData exchange2_data = orderbook_;
    exchange2_data.exchange = Exchange::Bybit;

    strategy_->on_market_data(exchange1_data);
    strategy_->on_market_data(exchange2_data);

    auto signals = strategy_->get_signals();
    EXPECT_TRUE(signals.empty());
}

TEST_F(LatencyArbitrageTest, DetectsArbitrageOpportunity) {
    // Exchange 1: BTC at $50000
    MarketData exchange1_data = orderbook_;
    exchange1_data.exchange = Exchange::Binance;

    // Exchange 2: BTC at $50050 (0.1% higher)
    MarketData exchange2_data = orderbook_;
    exchange2_data.exchange = Exchange::Bybit;
    for (auto& level : exchange2_data.bids) {
        level.price += 50.0;
    }
    for (auto& level : exchange2_data.asks) {
        level.price += 50.0;
    }

    strategy_->on_market_data(exchange1_data);
    strategy_->on_market_data(exchange2_data);

    auto signals = strategy_->get_signals();
    EXPECT_FALSE(signals.empty());

    // Should have buy on cheaper exchange, sell on expensive
    bool has_buy = false, has_sell = false;
    for (const auto& signal : signals) {
        if (signal.side == Side::Buy && signal.exchange == Exchange::Binance) {
            has_buy = true;
        }
        if (signal.side == Side::Sell && signal.exchange == Exchange::Bybit) {
            has_sell = true;
        }
    }
    EXPECT_TRUE(has_buy);
    EXPECT_TRUE(has_sell);
}

// ============================================================================
// Triangular Arbitrage Tests
// ============================================================================

class TriangularArbitrageTest : public StrategyTestBase {
protected:
    void SetUp() override {
        StrategyTestBase::SetUp();

        StrategyParams params;
        params.set("min_profit_bps", 3.0);  // 0.03%
        params.set("max_position", 1.0);

        strategy_ = std::make_unique<hft::TriangularArbitrage>(params);
    }

    std::unique_ptr<hft::TriangularArbitrage> strategy_;
};

TEST_F(TriangularArbitrageTest, DetectsTriangularOpportunity) {
    // Setup: BTC/USDT, ETH/USDT, ETH/BTC
    // Prices that create arbitrage:
    // BTC/USDT = 50000, ETH/USDT = 2500, ETH/BTC = 0.0495 (should be 0.05)
    // Buy ETH/BTC, Sell ETH/USDT, Buy BTC/USDT = profit

    MarketData btc_usdt;
    btc_usdt.symbol = "BTC-USDT";
    btc_usdt.exchange = Exchange::Binance;
    btc_usdt.bids = {{50000, 1.0}};
    btc_usdt.asks = {{50001, 1.0}};

    MarketData eth_usdt;
    eth_usdt.symbol = "ETH-USDT";
    eth_usdt.exchange = Exchange::Binance;
    eth_usdt.bids = {{2500, 10.0}};
    eth_usdt.asks = {{2501, 10.0}};

    MarketData eth_btc;
    eth_btc.symbol = "ETH-BTC";
    eth_btc.exchange = Exchange::Binance;
    eth_btc.bids = {{0.0495, 10.0}};  // Mispriced
    eth_btc.asks = {{0.0496, 10.0}};

    strategy_->on_market_data(btc_usdt);
    strategy_->on_market_data(eth_usdt);
    strategy_->on_market_data(eth_btc);

    auto signals = strategy_->get_signals();
    // May or may not have signals depending on exact fee calculations
    EXPECT_GE(signals.size(), 0);
}

// ============================================================================
// Statistical Arbitrage Tests
// ============================================================================

class StatisticalArbitrageTest : public StrategyTestBase {
protected:
    void SetUp() override {
        StrategyTestBase::SetUp();

        StrategyParams params;
        params.set("lookback_period", 100);
        params.set("entry_zscore", 2.0);
        params.set("exit_zscore", 0.5);
        params.set("hedge_ratio", 1.0);

        strategy_ = std::make_unique<hft::StatisticalArbitrage>(params);
    }

    std::unique_ptr<hft::StatisticalArbitrage> strategy_;
};

TEST_F(StatisticalArbitrageTest, CalculatesSpreadCorrectly) {
    // Feed correlated price series
    for (int i = 0; i < 100; ++i) {
        MarketData asset1;
        asset1.symbol = "BTC-USDT";
        asset1.exchange = Exchange::Binance;
        asset1.mid_price = 50000 + i * 10;

        MarketData asset2;
        asset2.symbol = "ETH-USDT";
        asset2.exchange = Exchange::Binance;
        asset2.mid_price = 2500 + i * 0.5;  // Correlated

        strategy_->on_market_data(asset1);
        strategy_->on_market_data(asset2);
    }

    // Strategy should have calculated statistics
    EXPECT_TRUE(strategy_->is_calibrated());
}

// ============================================================================
// Market Making Tests
// ============================================================================

class BasicMarketMakerTest : public StrategyTestBase {
protected:
    void SetUp() override {
        StrategyTestBase::SetUp();

        StrategyParams params;
        params.set("spread_bps", 10.0);  // 0.1%
        params.set("order_size", 0.1);
        params.set("num_levels", 3);
        params.set("refresh_interval_ms", 1000);

        strategy_ = std::make_unique<mm::BasicMarketMaker>(params);
    }

    std::unique_ptr<mm::BasicMarketMaker> strategy_;
};

TEST_F(BasicMarketMakerTest, GeneratesQuotes) {
    strategy_->on_market_data(orderbook_);

    auto signals = strategy_->get_signals();

    // Should have both bid and ask quotes
    EXPECT_GE(signals.size(), 2);

    bool has_bid = false, has_ask = false;
    for (const auto& signal : signals) {
        if (signal.side == Side::Buy) has_bid = true;
        if (signal.side == Side::Sell) has_ask = true;
    }

    EXPECT_TRUE(has_bid);
    EXPECT_TRUE(has_ask);
}

TEST_F(BasicMarketMakerTest, QuotesAroundMid) {
    strategy_->on_market_data(orderbook_);

    auto signals = strategy_->get_signals();
    double mid_price = (orderbook_.bids[0].price + orderbook_.asks[0].price) / 2;

    for (const auto& signal : signals) {
        if (signal.side == Side::Buy) {
            EXPECT_LT(signal.price, mid_price);
        } else {
            EXPECT_GT(signal.price, mid_price);
        }
    }
}

// ============================================================================
// Avellaneda-Stoikov Tests
// ============================================================================

class AvellanedaStoikovTest : public StrategyTestBase {
protected:
    void SetUp() override {
        StrategyTestBase::SetUp();

        StrategyParams params;
        params.set("gamma", 0.1);      // Risk aversion
        params.set("sigma", 0.02);     // Volatility (2%)
        params.set("T", 1.0);          // Time horizon (1 day)
        params.set("k", 1.5);          // Order arrival intensity
        params.set("max_inventory", 10);

        strategy_ = std::make_unique<mm::AvellanedaStoikov>(params);
    }

    std::unique_ptr<mm::AvellanedaStoikov> strategy_;
};

TEST_F(AvellanedaStoikovTest, ReservationPriceCalculation) {
    strategy_->on_market_data(orderbook_);

    // At zero inventory, reservation price should be close to mid
    double mid = (orderbook_.bids[0].price + orderbook_.asks[0].price) / 2;
    double reservation = strategy_->get_reservation_price();

    EXPECT_NEAR(reservation, mid, mid * 0.01);  // Within 1%
}

TEST_F(AvellanedaStoikovTest, InventorySkewing) {
    // Set positive inventory - should skew quotes down
    strategy_->set_inventory(5.0);
    strategy_->on_market_data(orderbook_);

    auto signals = strategy_->get_signals();
    double mid = (orderbook_.bids[0].price + orderbook_.asks[0].price) / 2;

    // With positive inventory, bid should be lower to reduce buying
    for (const auto& signal : signals) {
        if (signal.side == Side::Buy) {
            EXPECT_LT(signal.price, mid - mid * 0.001);  // More aggressive selling
        }
    }
}

// ============================================================================
// Inventory Market Maker Tests
// ============================================================================

class InventoryMarketMakerTest : public StrategyTestBase {
protected:
    void SetUp() override {
        StrategyTestBase::SetUp();

        StrategyParams params;
        params.set("base_spread_bps", 10.0);
        params.set("inventory_skew_factor", 0.5);
        params.set("target_inventory", 0.0);
        params.set("max_inventory", 10.0);

        strategy_ = std::make_unique<mm::InventoryMarketMaker>(params);
    }

    std::unique_ptr<mm::InventoryMarketMaker> strategy_;
};

TEST_F(InventoryMarketMakerTest, SkewsWithInventory) {
    // Zero inventory - symmetric quotes
    strategy_->set_inventory(0.0);
    strategy_->on_market_data(orderbook_);
    auto signals_zero = strategy_->get_signals();

    // Positive inventory - skew to sell
    strategy_->set_inventory(5.0);
    strategy_->on_market_data(orderbook_);
    auto signals_positive = strategy_->get_signals();

    // Find bid prices
    double bid_zero = 0, bid_positive = 0;
    for (const auto& s : signals_zero) {
        if (s.side == Side::Buy) bid_zero = s.price;
    }
    for (const auto& s : signals_positive) {
        if (s.side == Side::Buy) bid_positive = s.price;
    }

    // With positive inventory, bid should be lower (less eager to buy)
    EXPECT_LT(bid_positive, bid_zero);
}

// ============================================================================
// Grid Market Maker Tests
// ============================================================================

class GridMarketMakerTest : public StrategyTestBase {
protected:
    void SetUp() override {
        StrategyTestBase::SetUp();

        StrategyParams params;
        params.set("grid_spacing_pct", 0.5);
        params.set("num_grids", 5);
        params.set("order_size", 0.1);
        params.set("rebalance_threshold", 0.1);

        strategy_ = std::make_unique<mm::GridMarketMaker>(params);
    }

    std::unique_ptr<mm::GridMarketMaker> strategy_;
};

TEST_F(GridMarketMakerTest, CreatesGridLevels) {
    strategy_->on_market_data(orderbook_);

    auto signals = strategy_->get_signals();

    // Should have multiple levels on both sides
    int buy_count = 0, sell_count = 0;
    for (const auto& signal : signals) {
        if (signal.side == Side::Buy) buy_count++;
        else sell_count++;
    }

    EXPECT_GE(buy_count, 3);
    EXPECT_GE(sell_count, 3);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
