#pragma once

/**
 * @file trading_engine.hpp
 * @brief Main trading engine orchestrating all components
 *
 * This file implements the core trading engine that coordinates:
 * - Strategy registration and lifecycle management
 * - Market data distribution to strategies
 * - Order routing from strategies to the Order Management System
 * - Multi-threaded event loop with microsecond precision
 * - Mode switching between paper and live trading
 * - Graceful shutdown handling
 */

#include "trading/trading_mode.hpp"
#include "trading/paper_trading.hpp"
#include "trading/live_trading.hpp"
#include "core/types.hpp"

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <queue>
#include <chrono>

namespace hft::trading {

// ============================================================================
// Forward Declarations
// ============================================================================

class TradingEngine;
class Strategy;
using TradingEnginePtr = std::shared_ptr<TradingEngine>;
using StrategyPtr = std::shared_ptr<Strategy>;

// ============================================================================
// Market Data Types
// ============================================================================

/**
 * @brief Order book level (price + quantity)
 */
struct OrderBookLevel {
    core::Price price;
    core::Quantity quantity;
    uint32_t order_count{0};
};

/**
 * @brief Order book snapshot
 */
struct OrderBook {
    core::Exchange exchange{core::Exchange::Unknown};
    core::Symbol symbol;
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
    core::Timestamp timestamp;
    uint64_t sequence{0};

    [[nodiscard]] core::Price best_bid() const {
        return bids.empty() ? core::Price{0} : bids[0].price;
    }

    [[nodiscard]] core::Price best_ask() const {
        return asks.empty() ? core::Price{0} : asks[0].price;
    }

    [[nodiscard]] core::Price mid_price() const {
        if (bids.empty() || asks.empty()) {
            return core::Price{0};
        }
        return core::Price{(bids[0].price.value + asks[0].price.value) / 2};
    }

    [[nodiscard]] core::Price spread() const {
        if (bids.empty() || asks.empty()) {
            return core::Price{0};
        }
        return core::Price{asks[0].price.value - bids[0].price.value};
    }
};

/**
 * @brief Trade tick data
 */
struct TradeTick {
    core::Exchange exchange{core::Exchange::Unknown};
    core::Symbol symbol;
    core::TradeId trade_id;
    core::Price price;
    core::Quantity quantity;
    core::Side side{core::Side::Buy};
    core::Timestamp timestamp;
    bool is_buyer_maker{false};
};

/**
 * @brief Aggregated market data update
 */
struct MarketDataUpdate {
    enum class Type {
        OrderBook,
        Trade,
        Ticker
    };

    Type type{Type::OrderBook};
    core::Exchange exchange{core::Exchange::Unknown};
    core::Symbol symbol;

    // Order book data (if type == OrderBook)
    OrderBook order_book;

    // Trade data (if type == Trade)
    TradeTick trade;

    // Ticker data (common)
    core::Price last_price;
    core::Price best_bid;
    core::Price best_ask;
    core::Quantity volume_24h;
    core::Timestamp timestamp;
};

// ============================================================================
// Strategy Interface
// ============================================================================

/**
 * @brief Strategy state
 */
enum class StrategyState {
    Stopped,
    Starting,
    Running,
    Stopping,
    Error
};

/**
 * @brief Strategy configuration
 */
struct StrategyConfig {
    std::string name;
    std::string id;
    std::vector<core::Symbol> symbols;
    std::vector<core::Exchange> exchanges;
    bool enabled{true};
    uint32_t priority{0};              // Higher priority = process first
    uint64_t max_position_size{0};     // 0 = no limit
    uint64_t max_order_rate{0};        // Orders per second, 0 = no limit
    std::unordered_map<std::string, std::string> parameters;
};

/**
 * @brief Abstract strategy interface
 *
 * All trading strategies must implement this interface.
 */
class Strategy {
public:
    virtual ~Strategy() = default;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Initialize the strategy
     * @param config Strategy configuration
     * @param engine Reference to the trading engine
     * @return true if initialization successful
     */
    virtual bool initialize(const StrategyConfig& config,
                           TradingEngine* engine) = 0;

    /**
     * @brief Start the strategy
     */
    virtual bool start() = 0;

    /**
     * @brief Stop the strategy
     */
    virtual void stop() = 0;

    /**
     * @brief Get the strategy state
     */
    [[nodiscard]] virtual StrategyState state() const = 0;

    /**
     * @brief Get the strategy ID
     */
    [[nodiscard]] virtual const std::string& id() const = 0;

    /**
     * @brief Get the strategy name
     */
    [[nodiscard]] virtual const std::string& name() const = 0;

    // ========================================================================
    // Market Data Callbacks
    // ========================================================================

    /**
     * @brief Called when order book is updated
     */
    virtual void on_order_book(const OrderBook& book) = 0;

    /**
     * @brief Called when a trade occurs
     */
    virtual void on_trade(const TradeTick& trade) = 0;

    /**
     * @brief Called on market data update
     */
    virtual void on_market_data(const MarketDataUpdate& update) = 0;

    // ========================================================================
    // Execution Callbacks
    // ========================================================================

    /**
     * @brief Called when an order response is received
     */
    virtual void on_order_response(const OrderResponse& response) = 0;

    /**
     * @brief Called when an execution occurs
     */
    virtual void on_execution(const ExecutionReport& execution) = 0;

    /**
     * @brief Called when a position is updated
     */
    virtual void on_position_update(const Position& position) = 0;

    // ========================================================================
    // Timer Callback
    // ========================================================================

    /**
     * @brief Called periodically (configurable interval)
     */
    virtual void on_timer(core::Timestamp timestamp) = 0;
};

// ============================================================================
// Engine Event Types
// ============================================================================

/**
 * @brief Event types for the engine event loop
 */
enum class EventType {
    MarketData,
    OrderResponse,
    Execution,
    PositionUpdate,
    BalanceUpdate,
    Timer,
    Shutdown,
    Custom
};

/**
 * @brief Engine event wrapper
 */
struct EngineEvent {
    EventType type{EventType::Custom};
    core::Timestamp timestamp;

    // Event data (union-like, only one is valid based on type)
    MarketDataUpdate market_data;
    OrderResponse order_response;
    ExecutionReport execution;
    Position position;
    Balance balance;

    // Custom event data
    std::string custom_type;
    std::vector<uint8_t> custom_data;
};

// ============================================================================
// Thread Pinning Configuration
// ============================================================================

/**
 * @brief CPU affinity configuration for threads
 */
struct ThreadPinningConfig {
    bool enabled{false};
    int event_loop_core{-1};           // Core for main event loop
    int market_data_core{-1};          // Core for market data processing
    int order_routing_core{-1};        // Core for order routing
    std::vector<int> strategy_cores;   // Cores for strategy threads
};

// ============================================================================
// Engine Configuration
// ============================================================================

/**
 * @brief Trading engine configuration
 */
struct TradingEngineConfig {
    std::string name{"HFT_Engine"};

    // Trading mode
    core::TradingMode mode{core::TradingMode::Paper};
    TradingModeConfig mode_config;

    // Threading
    uint32_t event_queue_size{100000};
    uint32_t market_data_queue_size{100000};
    uint32_t order_queue_size{10000};
    uint32_t timer_interval_us{1000};  // Timer callback interval
    ThreadPinningConfig thread_pinning;

    // Event loop
    bool busy_poll{false};             // Use busy polling vs blocking wait
    uint32_t poll_timeout_us{100};     // Poll timeout if not busy polling

    // Shutdown
    uint32_t shutdown_timeout_ms{5000};
    bool cancel_on_shutdown{true};     // Cancel all orders on shutdown

    // Logging
    bool log_events{false};
    bool log_orders{true};
    bool log_executions{true};
};

// ============================================================================
// Trading Engine
// ============================================================================

/**
 * @brief Main trading engine
 *
 * Orchestrates all trading components:
 * - Receives market data and distributes to strategies
 * - Receives orders from strategies and routes to OMS
 * - Manages strategy lifecycle
 * - Provides microsecond-precision event loop
 */
class TradingEngine : public std::enable_shared_from_this<TradingEngine> {
public:
    TradingEngine();
    ~TradingEngine();

    // Non-copyable
    TradingEngine(const TradingEngine&) = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;

    // ========================================================================
    // Lifecycle Management
    // ========================================================================

    /**
     * @brief Initialize the engine
     * @param config Engine configuration
     * @return true if initialization successful
     */
    bool initialize(const TradingEngineConfig& config);

    /**
     * @brief Start the engine
     * @return true if started successfully
     */
    bool start();

    /**
     * @brief Stop the engine gracefully
     */
    void stop();

    /**
     * @brief Wait for the engine to stop
     */
    void wait();

    /**
     * @brief Check if the engine is running
     */
    [[nodiscard]] bool is_running() const;

    /**
     * @brief Request shutdown
     */
    void request_shutdown();

    // ========================================================================
    // Strategy Management
    // ========================================================================

    /**
     * @brief Register a strategy
     * @param strategy Strategy instance
     * @param config Strategy configuration
     * @return true if registration successful
     */
    bool register_strategy(StrategyPtr strategy, const StrategyConfig& config);

    /**
     * @brief Unregister a strategy
     * @param strategy_id Strategy ID
     * @return true if unregistration successful
     */
    bool unregister_strategy(const std::string& strategy_id);

    /**
     * @brief Get a strategy by ID
     */
    [[nodiscard]] StrategyPtr get_strategy(const std::string& strategy_id) const;

    /**
     * @brief Get all registered strategies
     */
    [[nodiscard]] std::vector<StrategyPtr> get_all_strategies() const;

    /**
     * @brief Start a specific strategy
     */
    bool start_strategy(const std::string& strategy_id);

    /**
     * @brief Stop a specific strategy
     */
    bool stop_strategy(const std::string& strategy_id);

    // ========================================================================
    // Trading Mode Management
    // ========================================================================

    /**
     * @brief Get the current trading mode
     */
    [[nodiscard]] TradingModePtr get_trading_mode() const;

    /**
     * @brief Switch trading mode (requires engine stop)
     * @param mode New trading mode type
     * @param config Configuration for the new mode
     * @return true if switch successful
     */
    bool switch_mode(core::TradingMode mode, const TradingModeConfig& config);

    /**
     * @brief Check if in paper trading mode
     */
    [[nodiscard]] bool is_paper_trading() const;

    /**
     * @brief Check if in live trading mode
     */
    [[nodiscard]] bool is_live_trading() const;

    // ========================================================================
    // Market Data Interface
    // ========================================================================

    /**
     * @brief Process incoming order book update
     * @param book Order book data
     */
    void on_order_book(const OrderBook& book);

    /**
     * @brief Process incoming trade
     * @param trade Trade data
     */
    void on_trade(const TradeTick& trade);

    /**
     * @brief Process incoming market data update
     * @param update Market data update
     */
    void on_market_data(const MarketDataUpdate& update);

    /**
     * @brief Subscribe to a symbol
     * @param exchange Exchange
     * @param symbol Symbol
     */
    void subscribe(core::Exchange exchange, const core::Symbol& symbol);

    /**
     * @brief Unsubscribe from a symbol
     * @param exchange Exchange
     * @param symbol Symbol
     */
    void unsubscribe(core::Exchange exchange, const core::Symbol& symbol);

    // ========================================================================
    // Order Interface (for strategies)
    // ========================================================================

    /**
     * @brief Submit an order
     * @param request Order request
     * @return Order response
     */
    OrderResponse submit_order(const OrderRequest& request);

    /**
     * @brief Cancel an order
     * @param request Cancel request
     * @return Cancel response
     */
    CancelResponse cancel_order(const CancelRequest& request);

    /**
     * @brief Cancel all orders
     * @param exchange Exchange (Unknown = all)
     * @param symbol Symbol (empty = all)
     * @return Number cancelled
     */
    uint32_t cancel_all_orders(core::Exchange exchange = core::Exchange::Unknown,
                               const core::Symbol& symbol = core::Symbol{});

    /**
     * @brief Get position
     */
    [[nodiscard]] std::optional<Position> get_position(
        core::Exchange exchange, const core::Symbol& symbol) const;

    /**
     * @brief Get all positions
     */
    [[nodiscard]] std::vector<Position> get_all_positions(
        core::Exchange exchange = core::Exchange::Unknown) const;

    /**
     * @brief Get balance
     */
    [[nodiscard]] std::optional<Balance> get_balance(
        core::Exchange exchange, const std::string& asset) const;

    /**
     * @brief Get all balances
     */
    [[nodiscard]] std::vector<Balance> get_all_balances(
        core::Exchange exchange = core::Exchange::Unknown) const;

    // ========================================================================
    // Event Queue Interface
    // ========================================================================

    /**
     * @brief Push an event to the queue
     * @param event Event to push
     * @return true if event was queued
     */
    bool push_event(EngineEvent event);

    /**
     * @brief Get current timestamp
     */
    [[nodiscard]] core::Timestamp current_time() const;

    // ========================================================================
    // Statistics and Diagnostics
    // ========================================================================

    /**
     * @brief Get engine configuration
     */
    [[nodiscard]] const TradingEngineConfig& config() const;

    /**
     * @brief Get event processing latency statistics
     */
    struct LatencyStats {
        uint64_t count{0};
        double mean_us{0.0};
        double median_us{0.0};
        double p99_us{0.0};
        double max_us{0.0};
    };

    [[nodiscard]] LatencyStats get_latency_stats() const;

    /**
     * @brief Get events processed per second
     */
    [[nodiscard]] double events_per_second() const;

    /**
     * @brief Get current queue depths
     */
    struct QueueDepths {
        size_t event_queue{0};
        size_t market_data_queue{0};
        size_t order_queue{0};
    };

    [[nodiscard]] QueueDepths get_queue_depths() const;

    /**
     * @brief Reset statistics
     */
    void reset_statistics();

private:
    // ========================================================================
    // Internal Methods
    // ========================================================================

    /**
     * @brief Main event loop
     */
    void event_loop();

    /**
     * @brief Market data processing loop
     */
    void market_data_loop();

    /**
     * @brief Order routing loop
     */
    void order_routing_loop();

    /**
     * @brief Timer loop
     */
    void timer_loop();

    /**
     * @brief Process a single event
     */
    void process_event(const EngineEvent& event);

    /**
     * @brief Distribute market data to strategies
     */
    void distribute_market_data(const MarketDataUpdate& update);

    /**
     * @brief Distribute order response to strategies
     */
    void distribute_order_response(const OrderResponse& response);

    /**
     * @brief Distribute execution to strategies
     */
    void distribute_execution(const ExecutionReport& execution);

    /**
     * @brief Distribute position update to strategies
     */
    void distribute_position_update(const Position& position);

    /**
     * @brief Fire timer callbacks
     */
    void fire_timers();

    /**
     * @brief Set thread affinity
     */
    void set_thread_affinity(int core);

    /**
     * @brief Update latency statistics
     */
    void update_latency_stats(uint64_t latency_ns);

    /**
     * @brief Handle trading mode callbacks
     */
    void setup_mode_callbacks();

    // ========================================================================
    // Member Variables
    // ========================================================================

    // Configuration
    TradingEngineConfig config_;

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutdown_requested_{false};

    // Trading mode
    TradingModePtr trading_mode_;
    std::shared_ptr<PaperTradingMode> paper_mode_;
    std::shared_ptr<LiveTradingMode> live_mode_;

    // Strategies
    struct StrategyEntry {
        StrategyPtr strategy;
        StrategyConfig config;
        StrategyState state{StrategyState::Stopped};
    };
    std::unordered_map<std::string, StrategyEntry> strategies_;
    mutable std::shared_mutex strategies_mutex_;

    // Event queue
    struct EventQueue {
        std::queue<EngineEvent> queue;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<size_t> size{0};
    };
    EventQueue event_queue_;
    EventQueue market_data_queue_;
    EventQueue order_queue_;

    // Threads
    std::unique_ptr<std::thread> event_thread_;
    std::unique_ptr<std::thread> market_data_thread_;
    std::unique_ptr<std::thread> order_thread_;
    std::unique_ptr<std::thread> timer_thread_;

    // Subscriptions
    struct SubscriptionKey {
        core::Exchange exchange;
        core::Symbol symbol;

        bool operator==(const SubscriptionKey& other) const {
            return exchange == other.exchange && symbol == other.symbol;
        }
    };
    struct SubscriptionKeyHash {
        std::size_t operator()(const SubscriptionKey& k) const {
            return std::hash<uint8_t>{}(static_cast<uint8_t>(k.exchange)) ^
                   (std::hash<std::string_view>{}(k.symbol.view()) << 1);
        }
    };
    std::unordered_map<SubscriptionKey, std::vector<std::string>,
                       SubscriptionKeyHash> subscriptions_;
    std::mutex subscriptions_mutex_;

    // Order tracking (for routing responses back to strategies)
    std::unordered_map<uint64_t, std::string> order_to_strategy_;
    std::mutex order_tracking_mutex_;

    // Latency statistics
    struct LatencyStatsInternal {
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> total_ns{0};
        std::atomic<uint64_t> max_ns{0};
        std::vector<uint64_t> samples;
        mutable std::mutex mutex;
    };
    LatencyStatsInternal latency_stats_;

    // Event rate tracking
    std::atomic<uint64_t> events_processed_{0};
    std::chrono::steady_clock::time_point start_time_;

    // Shutdown synchronization
    std::mutex shutdown_mutex_;
    std::condition_variable shutdown_cv_;
};

// ============================================================================
// Factory Functions
// ============================================================================

/**
 * @brief Create a trading engine instance
 */
[[nodiscard]] TradingEnginePtr create_trading_engine();

/**
 * @brief Create default engine configuration
 */
[[nodiscard]] TradingEngineConfig create_default_engine_config();

// ============================================================================
// Base Strategy Implementation
// ============================================================================

/**
 * @brief Base strategy implementation with common functionality
 */
class BaseStrategy : public Strategy {
public:
    BaseStrategy();
    ~BaseStrategy() override;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    bool initialize(const StrategyConfig& config,
                   TradingEngine* engine) override;
    bool start() override;
    void stop() override;
    [[nodiscard]] StrategyState state() const override;
    [[nodiscard]] const std::string& id() const override;
    [[nodiscard]] const std::string& name() const override;

    // ========================================================================
    // Default Implementations (override as needed)
    // ========================================================================

    void on_order_book(const OrderBook& book) override;
    void on_trade(const TradeTick& trade) override;
    void on_market_data(const MarketDataUpdate& update) override;
    void on_order_response(const OrderResponse& response) override;
    void on_execution(const ExecutionReport& execution) override;
    void on_position_update(const Position& position) override;
    void on_timer(core::Timestamp timestamp) override;

protected:
    // ========================================================================
    // Helper Methods for Derived Strategies
    // ========================================================================

    /**
     * @brief Submit an order through the engine
     */
    OrderResponse send_order(const OrderRequest& request);

    /**
     * @brief Cancel an order through the engine
     */
    CancelResponse send_cancel(const CancelRequest& request);

    /**
     * @brief Get current position
     */
    std::optional<Position> position(core::Exchange exchange,
                                     const core::Symbol& symbol) const;

    /**
     * @brief Get current balance
     */
    std::optional<Balance> balance(core::Exchange exchange,
                                   const std::string& asset) const;

    /**
     * @brief Log a message
     */
    void log(const std::string& message);

    // Configuration
    StrategyConfig config_;

    // Engine reference
    TradingEngine* engine_{nullptr};

    // State
    std::atomic<StrategyState> state_{StrategyState::Stopped};
};

}  // namespace hft::trading
