#pragma once

/**
 * @file simulated_exchange.hpp
 * @brief Simulated exchange for realistic backtesting
 *
 * Implements a realistic exchange simulation with order matching,
 * latency modeling, slippage, fees, and order book impact.
 * Provides the same interface as real exchange connectors.
 */

#include "core/types.hpp"
#include "backtesting/data_feed.hpp"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <map>
#include <queue>
#include <random>
#include <atomic>
#include <mutex>
#include <optional>

namespace hft::backtesting {

// ============================================================================
// Order Structures
// ============================================================================

/**
 * @brief Internal order representation for simulation
 */
struct SimulatedOrder {
    core::OrderId order_id;
    core::OrderId client_order_id;  // Client-assigned ID
    core::Exchange exchange;
    core::Symbol symbol;

    core::OrderType type;
    core::Side side;
    core::Price price;
    core::Price stop_price;         // For stop orders
    core::Quantity original_quantity;
    core::Quantity filled_quantity;
    core::Quantity remaining_quantity;

    core::OrderStatus status;
    core::Timestamp created_time;
    core::Timestamp updated_time;

    // Execution details
    core::Price average_fill_price;
    double total_fees{0.0};
    std::string fee_currency;

    // Simulation metadata
    core::Timestamp visible_time;    // When order becomes visible in book
    bool is_post_only{false};
    bool reduce_only{false};
    uint32_t fill_count{0};

    [[nodiscard]] double filled_value() const noexcept {
        return average_fill_price.to_double() * filled_quantity.to_double();
    }

    [[nodiscard]] bool is_active() const noexcept {
        return core::is_active_status(status);
    }
};

/**
 * @brief Fill/execution report
 */
struct Fill {
    core::OrderId order_id;
    core::TradeId trade_id;
    core::Exchange exchange;
    core::Symbol symbol;

    core::Side side;
    core::Price price;
    core::Quantity quantity;
    double fee{0.0};
    std::string fee_currency;

    core::Timestamp timestamp;
    bool is_maker{false};  // Was this a maker fill?

    [[nodiscard]] double notional() const noexcept {
        return price.to_double() * quantity.to_double();
    }
};

// ============================================================================
// Exchange Configuration
// ============================================================================

/**
 * @brief Fee structure configuration
 */
struct FeeConfig {
    double maker_fee{0.0001};        // 0.01% maker fee
    double taker_fee{0.0005};        // 0.05% taker fee
    double maker_rebate{0.0};        // Negative fee for makers
    std::string fee_currency{"USDT"};

    // Volume-based fee tiers (optional)
    struct FeeTier {
        double min_volume{0.0};
        double maker_fee;
        double taker_fee;
    };
    std::vector<FeeTier> tiers;

    [[nodiscard]] double getMakerFee(double volume_30d = 0.0) const;
    [[nodiscard]] double getTakerFee(double volume_30d = 0.0) const;
};

/**
 * @brief Latency simulation configuration
 */
struct LatencyConfig {
    // Order submission latency
    uint64_t order_submit_mean_us{500};      // Mean latency in microseconds
    uint64_t order_submit_stddev_us{100};    // Standard deviation
    uint64_t order_submit_min_us{100};       // Minimum latency
    uint64_t order_submit_max_us{5000};      // Maximum latency

    // Order cancellation latency
    uint64_t cancel_mean_us{300};
    uint64_t cancel_stddev_us{50};
    uint64_t cancel_min_us{50};
    uint64_t cancel_max_us{2000};

    // Market data latency
    uint64_t market_data_mean_us{100};
    uint64_t market_data_stddev_us{20};

    // Network jitter simulation
    bool enable_jitter{true};
    double jitter_probability{0.05};         // 5% chance of extra latency spike
    uint64_t jitter_max_us{10000};           // Max jitter spike

    // Queue time simulation
    uint64_t queue_processing_us{10};        // Per-order queue processing time
};

/**
 * @brief Slippage model configuration
 */
struct SlippageConfig {
    enum class Model {
        None,           // No slippage
        Fixed,          // Fixed basis points
        Linear,         // Linear in order size
        SquareRoot,     // Square root impact model
        OrderBook       // Use actual order book depth
    };

    Model model{Model::OrderBook};

    // Fixed model parameters
    double fixed_slippage_bps{1.0};  // 0.1 basis points

    // Linear model: slippage = base + coefficient * (size / adv)
    double linear_base_bps{0.5};
    double linear_coefficient{2.0};

    // Square root model: slippage = coefficient * sqrt(size / adv)
    double sqrt_coefficient{0.1};

    // Order book model settings
    bool use_actual_liquidity{true};
    double liquidity_factor{1.0};    // Multiply available liquidity
};

/**
 * @brief Complete simulated exchange configuration
 */
struct SimulatedExchangeConfig {
    core::Exchange exchange{core::Exchange::Binance};
    std::string name{"SimulatedExchange"};

    FeeConfig fees;
    LatencyConfig latency;
    SlippageConfig slippage;

    // Order limits
    core::Quantity min_order_size;
    core::Quantity max_order_size;
    core::Price min_price_increment;  // Tick size
    core::Quantity quantity_precision;

    // Partial fill simulation
    bool enable_partial_fills{true};
    double partial_fill_probability{0.3};  // 30% chance of partial fill
    double min_partial_fill_ratio{0.1};    // Minimum 10% of order

    // Order rejection simulation
    double reject_probability{0.001};       // 0.1% chance of random rejection
    bool reject_self_match{true};           // Reject self-matching orders

    // Order book simulation
    uint32_t order_book_depth{50};
    bool enable_order_book_impact{true};

    // Position limits (0 = unlimited)
    double max_position_size{0.0};
    double max_notional_value{0.0};

    // Rate limiting
    uint32_t max_orders_per_second{100};
    uint32_t max_cancels_per_second{100};
};

// ============================================================================
// Order Book Simulation
// ============================================================================

/**
 * @brief Simulated order book with realistic matching
 */
class SimulatedOrderBook {
public:
    explicit SimulatedOrderBook(const core::Symbol& symbol);

    // Update from market data
    void applySnapshot(const OrderBookSnapshot& snapshot);
    void applyUpdate(const OrderBookUpdate& update);

    // Order matching
    struct MatchResult {
        std::vector<Fill> fills;
        core::Quantity remaining_quantity;
        bool would_cross_spread{false};
        bool rejected{false};
        std::string reject_reason;
    };

    [[nodiscard]] MatchResult matchMarketOrder(
        const SimulatedOrder& order,
        core::Timestamp timestamp,
        const SlippageConfig& slippage);

    [[nodiscard]] MatchResult matchLimitOrder(
        const SimulatedOrder& order,
        core::Timestamp timestamp,
        const SlippageConfig& slippage);

    // Insert passive order into book
    void insertOrder(const SimulatedOrder& order);
    void removeOrder(core::OrderId order_id);

    // Queries
    [[nodiscard]] core::Price bestBid() const;
    [[nodiscard]] core::Price bestAsk() const;
    [[nodiscard]] core::Price midPrice() const;
    [[nodiscard]] double spreadBps() const;

    [[nodiscard]] core::Quantity bidQuantityAtPrice(core::Price price) const;
    [[nodiscard]] core::Quantity askQuantityAtPrice(core::Price price) const;
    [[nodiscard]] core::Quantity totalBidQuantity(uint32_t levels = 10) const;
    [[nodiscard]] core::Quantity totalAskQuantity(uint32_t levels = 10) const;

    [[nodiscard]] const OrderBookSnapshot& snapshot() const { return current_snapshot_; }

private:
    core::Symbol symbol_;
    OrderBookSnapshot current_snapshot_;

    // Our simulated orders in the book
    struct BookOrder {
        core::OrderId order_id;
        core::Price price;
        core::Quantity quantity;
        core::Timestamp time;
    };

    std::map<core::Price, std::vector<BookOrder>, std::greater<core::Price>> bid_orders_;
    std::map<core::Price, std::vector<BookOrder>> ask_orders_;

    uint64_t next_trade_id_{1};
    mutable std::mutex mutex_;

    [[nodiscard]] core::Quantity calculateSlippage(
        core::Quantity order_size,
        core::Side side,
        const SlippageConfig& config) const;
};

// ============================================================================
// Position Tracking
// ============================================================================

/**
 * @brief Position state for a symbol
 */
struct Position {
    core::Symbol symbol;
    double quantity{0.0};           // Signed: positive = long, negative = short
    double average_entry_price{0.0};
    double realized_pnl{0.0};
    double unrealized_pnl{0.0};
    double total_fees{0.0};

    core::Timestamp opened_time;
    core::Timestamp updated_time;

    [[nodiscard]] double notional_value(double current_price) const noexcept {
        return std::abs(quantity) * current_price;
    }

    [[nodiscard]] bool is_long() const noexcept { return quantity > 0; }
    [[nodiscard]] bool is_short() const noexcept { return quantity < 0; }
    [[nodiscard]] bool is_flat() const noexcept { return std::abs(quantity) < 1e-10; }
};

/**
 * @brief Account balances
 */
struct AccountBalance {
    std::string currency;
    double total{0.0};
    double available{0.0};
    double locked{0.0};            // In open orders

    [[nodiscard]] double free() const noexcept { return available; }
};

// ============================================================================
// Event Callbacks
// ============================================================================

using OnOrderCallback = std::function<void(const SimulatedOrder&)>;
using OnFillCallback = std::function<void(const Fill&)>;
using OnPositionCallback = std::function<void(const Position&)>;
using OnBalanceCallback = std::function<void(const AccountBalance&)>;
using OnErrorCallback = std::function<void(core::ErrorCode, const std::string&)>;

// ============================================================================
// Main Simulated Exchange Class
// ============================================================================

/**
 * @brief Main simulated exchange implementation
 *
 * Provides realistic exchange simulation with:
 * - Order matching engine
 * - Latency simulation
 * - Slippage modeling
 * - Fee calculation
 * - Partial fills
 * - Order book impact
 * - Position and balance tracking
 */
class SimulatedExchange {
public:
    explicit SimulatedExchange(const SimulatedExchangeConfig& config);
    ~SimulatedExchange();

    // Non-copyable
    SimulatedExchange(const SimulatedExchange&) = delete;
    SimulatedExchange& operator=(const SimulatedExchange&) = delete;

    // Initialize with starting balances
    void initialize(const std::unordered_map<std::string, double>& initial_balances);
    void reset();

    // Time management (called by backtest engine)
    void setCurrentTime(core::Timestamp time);
    [[nodiscard]] core::Timestamp currentTime() const noexcept { return current_time_; }

    // Market data updates
    void onOrderBookSnapshot(const OrderBookSnapshot& snapshot);
    void onOrderBookUpdate(const OrderBookUpdate& update);
    void onTrade(const Trade& trade);

    // Order management
    [[nodiscard]] core::OrderId submitOrder(
        const core::Symbol& symbol,
        core::Side side,
        core::OrderType type,
        core::Quantity quantity,
        core::Price price = core::Price{0},
        core::Price stop_price = core::Price{0},
        bool post_only = false,
        bool reduce_only = false,
        core::OrderId client_order_id = core::OrderId::invalid());

    [[nodiscard]] bool cancelOrder(core::OrderId order_id);
    [[nodiscard]] bool cancelAllOrders(const core::Symbol& symbol = core::Symbol{});
    [[nodiscard]] bool modifyOrder(
        core::OrderId order_id,
        core::Quantity new_quantity,
        core::Price new_price);

    // Order queries
    [[nodiscard]] std::optional<SimulatedOrder> getOrder(core::OrderId order_id) const;
    [[nodiscard]] std::vector<SimulatedOrder> getOpenOrders(
        const core::Symbol& symbol = core::Symbol{}) const;
    [[nodiscard]] std::vector<SimulatedOrder> getOrderHistory(
        const core::Symbol& symbol = core::Symbol{},
        size_t limit = 100) const;
    [[nodiscard]] std::vector<Fill> getFills(
        const core::Symbol& symbol = core::Symbol{},
        size_t limit = 100) const;

    // Position queries
    [[nodiscard]] Position getPosition(const core::Symbol& symbol) const;
    [[nodiscard]] std::vector<Position> getAllPositions() const;

    // Balance queries
    [[nodiscard]] AccountBalance getBalance(const std::string& currency) const;
    [[nodiscard]] std::unordered_map<std::string, AccountBalance> getAllBalances() const;
    [[nodiscard]] double getTotalEquity(const std::string& quote_currency = "USDT") const;

    // Market data queries
    [[nodiscard]] std::optional<OrderBookSnapshot> getOrderBook(const core::Symbol& symbol) const;
    [[nodiscard]] core::Price getLastPrice(const core::Symbol& symbol) const;
    [[nodiscard]] core::Price getMidPrice(const core::Symbol& symbol) const;

    // Statistics
    [[nodiscard]] uint64_t totalOrdersSubmitted() const noexcept { return total_orders_; }
    [[nodiscard]] uint64_t totalOrdersFilled() const noexcept { return total_fills_; }
    [[nodiscard]] uint64_t totalOrdersRejected() const noexcept { return total_rejects_; }
    [[nodiscard]] double totalFeessPaid() const noexcept { return total_fees_paid_; }
    [[nodiscard]] double totalVolume() const noexcept { return total_volume_; }

    // Callbacks
    void setOnOrder(OnOrderCallback callback) { on_order_ = std::move(callback); }
    void setOnFill(OnFillCallback callback) { on_fill_ = std::move(callback); }
    void setOnPosition(OnPositionCallback callback) { on_position_ = std::move(callback); }
    void setOnBalance(OnBalanceCallback callback) { on_balance_ = std::move(callback); }
    void setOnError(OnErrorCallback callback) { on_error_ = std::move(callback); }

    // Configuration access
    [[nodiscard]] const SimulatedExchangeConfig& config() const { return config_; }
    void updateConfig(const SimulatedExchangeConfig& config) { config_ = config; }

    // Process pending events (called by backtest engine each tick)
    void processEvents();

private:
    // Internal order processing
    void processOrder(SimulatedOrder& order);
    void executeMarketOrder(SimulatedOrder& order);
    void executeLimitOrder(SimulatedOrder& order);
    void executeStopOrder(SimulatedOrder& order);

    // Position and balance updates
    void updatePosition(const core::Symbol& symbol, const Fill& fill);
    void updateBalance(const std::string& currency, double delta, bool is_available);
    void lockBalance(const std::string& currency, double amount);
    void unlockBalance(const std::string& currency, double amount);

    // Fee calculation
    [[nodiscard]] double calculateFee(const Fill& fill) const;

    // Latency simulation
    [[nodiscard]] core::Timestamp simulateLatency(uint64_t mean_us, uint64_t stddev_us,
                                                   uint64_t min_us, uint64_t max_us);

    // Validation
    [[nodiscard]] bool validateOrder(const SimulatedOrder& order, std::string& error) const;
    [[nodiscard]] bool checkRateLimit();
    [[nodiscard]] bool checkPositionLimit(const core::Symbol& symbol, double additional_size) const;

    // Event emission
    void emitOrderUpdate(const SimulatedOrder& order);
    void emitFill(const Fill& fill);
    void emitPositionUpdate(const Position& position);
    void emitBalanceUpdate(const AccountBalance& balance);
    void emitError(core::ErrorCode code, const std::string& message);

    // Configuration
    SimulatedExchangeConfig config_;

    // Time management
    core::Timestamp current_time_;

    // Order management
    std::unordered_map<uint64_t, SimulatedOrder> orders_;
    std::vector<SimulatedOrder> order_history_;
    std::vector<Fill> fill_history_;
    uint64_t next_order_id_{1};

    // Pending order events (for latency simulation)
    struct PendingEvent {
        core::Timestamp execute_time;
        std::function<void()> action;

        bool operator>(const PendingEvent& other) const {
            return execute_time > other.execute_time;
        }
    };
    std::priority_queue<PendingEvent, std::vector<PendingEvent>,
                        std::greater<PendingEvent>> pending_events_;

    // Order books per symbol
    std::unordered_map<std::string, std::unique_ptr<SimulatedOrderBook>> order_books_;

    // Positions per symbol
    std::unordered_map<std::string, Position> positions_;

    // Balances per currency
    std::unordered_map<std::string, AccountBalance> balances_;

    // Last prices per symbol
    std::unordered_map<std::string, core::Price> last_prices_;

    // Statistics
    std::atomic<uint64_t> total_orders_{0};
    std::atomic<uint64_t> total_fills_{0};
    std::atomic<uint64_t> total_rejects_{0};
    std::atomic<double> total_fees_paid_{0.0};
    std::atomic<double> total_volume_{0.0};

    // Rate limiting
    std::deque<core::Timestamp> order_timestamps_;
    std::deque<core::Timestamp> cancel_timestamps_;

    // Random number generation for simulation
    std::mt19937_64 rng_;
    std::normal_distribution<double> latency_dist_;

    // Callbacks
    OnOrderCallback on_order_;
    OnFillCallback on_fill_;
    OnPositionCallback on_position_;
    OnBalanceCallback on_balance_;
    OnErrorCallback on_error_;

    // Thread safety
    mutable std::mutex mutex_;
};

// ============================================================================
// Exchange Factory
// ============================================================================

/**
 * @brief Factory for creating simulated exchanges
 */
class SimulatedExchangeFactory {
public:
    // Create with default config for specific exchange
    [[nodiscard]] static std::unique_ptr<SimulatedExchange> create(core::Exchange exchange);

    // Create with custom config
    [[nodiscard]] static std::unique_ptr<SimulatedExchange> create(
        const SimulatedExchangeConfig& config);

    // Get default config for exchange
    [[nodiscard]] static SimulatedExchangeConfig getDefaultConfig(core::Exchange exchange);
};

}  // namespace hft::backtesting
