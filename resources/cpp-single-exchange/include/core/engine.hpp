#pragma once

#include "core/types.hpp"
#include "core/lock_free_queue.hpp"
#include "core/memory_pool.hpp"
#include "orderbook/orderbook.hpp"
#include "strategy/market_maker.hpp"
#include "risk/risk_manager.hpp"
#include "exchange/exchange_client.hpp"
#include "utils/logger.hpp"
#include "utils/config.hpp"

#include <atomic>
#include <thread>
#include <memory>
#include <functional>

namespace hft {

// ============================================================================
// Event Types for Inter-Thread Communication
// ============================================================================

enum class EventType : uint8_t {
    MARKET_DATA,
    ORDER_UPDATE,
    TRADE,
    POSITION_UPDATE,
    RISK_ALERT,
    SHUTDOWN
};

struct Event {
    EventType type;
    Timestamp timestamp;
    union {
        Tick tick;
        Order order;
        Trade trade;
    } data;
};

// ============================================================================
// Trading Engine - Core HFT System
// ============================================================================

class TradingEngine {
public:
    explicit TradingEngine(const Config& config);
    ~TradingEngine();

    // Non-copyable, non-movable
    TradingEngine(const TradingEngine&) = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;

    // Lifecycle
    void start();
    void stop();
    [[nodiscard]] bool is_running() const noexcept;

    // Event handling
    void on_market_data(const Tick& tick);
    void on_order_update(const Order& order);
    void on_trade(const Trade& trade);

    // Order management
    OrderId send_order(const Order& order);
    bool cancel_order(OrderId order_id);
    bool modify_order(OrderId order_id, Price new_price, Quantity new_qty);

    // Getters
    [[nodiscard]] const OrderBook& orderbook() const noexcept { return *orderbook_; }
    [[nodiscard]] const RiskManager& risk_manager() const noexcept { return *risk_manager_; }
    [[nodiscard]] const MarketMaker& strategy() const noexcept { return *strategy_; }

private:
    // Thread functions
    void market_data_thread();
    void strategy_thread();
    void order_thread();
    void risk_thread();

    // Event processing
    void process_market_data(const Tick& tick);
    void process_order_update(const Order& order);
    void process_trade(const Trade& trade);

    // Configuration
    Config config_;

    // Core components
    std::unique_ptr<OrderBook> orderbook_;
    std::unique_ptr<MarketMaker> strategy_;
    std::unique_ptr<RiskManager> risk_manager_;
    std::unique_ptr<ExchangeClient> exchange_client_;
    std::unique_ptr<Logger> logger_;

    // Lock-free queues for inter-thread communication
    LockFreeQueue<Tick, 65536> market_data_queue_;
    LockFreeQueue<Order, 8192> order_queue_;
    LockFreeQueue<Trade, 8192> trade_queue_;
    LockFreeQueue<Event, 4096> event_queue_;

    // Memory pools
    MemoryPool<Order, 10000> order_pool_;

    // Threads
    std::thread market_data_thread_;
    std::thread strategy_thread_;
    std::thread order_thread_;
    std::thread risk_thread_;

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> trading_enabled_{false};
    std::atomic<uint64_t> order_id_counter_{0};

    // Performance metrics
    std::atomic<uint64_t> ticks_processed_{0};
    std::atomic<uint64_t> orders_sent_{0};
    std::atomic<uint64_t> trades_executed_{0};
};

// ============================================================================
// Engine Builder - Fluent API for configuration
// ============================================================================

class EngineBuilder {
public:
    EngineBuilder& with_config(const std::string& config_path);
    EngineBuilder& with_symbol(const std::string& symbol);
    EngineBuilder& with_exchange(const std::string& exchange);
    EngineBuilder& with_strategy(std::unique_ptr<MarketMaker> strategy);
    EngineBuilder& with_risk_limits(const RiskLimits& limits);

    [[nodiscard]] std::unique_ptr<TradingEngine> build();

private:
    Config config_;
    std::string symbol_;
    std::string exchange_;
    std::unique_ptr<MarketMaker> strategy_;
    RiskLimits risk_limits_;
};

} // namespace hft
