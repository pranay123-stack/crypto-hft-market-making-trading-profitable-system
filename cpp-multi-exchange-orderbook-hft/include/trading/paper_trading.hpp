#pragma once

/**
 * @file paper_trading.hpp
 * @brief Paper trading mode implementation
 *
 * This file implements simulated trading using real market data.
 * Perfect for strategy testing and development without risking real capital.
 */

#include "trading/trading_mode.hpp"

#include <unordered_map>
#include <map>
#include <deque>
#include <shared_mutex>
#include <thread>
#include <condition_variable>
#include <random>

namespace hft::trading {

// ============================================================================
// Paper Trading Order Book
// ============================================================================

/**
 * @brief Simulated order for paper trading
 */
struct SimulatedOrder {
    OrderRequest request;
    core::OrderId exchange_order_id;
    core::OrderStatus status{core::OrderStatus::New};
    core::Quantity filled_quantity;
    core::Price avg_fill_price;
    core::Timestamp created_at;
    core::Timestamp updated_at;
    uint32_t fill_count{0};                  // Number of fills
};

/**
 * @brief Market price snapshot for fill simulation
 */
struct MarketPriceSnapshot {
    core::Price bid;
    core::Price ask;
    core::Price last_price;
    core::Timestamp timestamp;
    bool valid{false};
};

/**
 * @brief Symbol key for map lookups
 */
struct SymbolKey {
    core::Exchange exchange;
    core::Symbol symbol;

    bool operator==(const SymbolKey& other) const {
        return exchange == other.exchange && symbol == other.symbol;
    }
};

struct SymbolKeyHash {
    std::size_t operator()(const SymbolKey& k) const {
        return std::hash<uint8_t>{}(static_cast<uint8_t>(k.exchange)) ^
               (std::hash<std::string_view>{}(k.symbol.view()) << 1);
    }
};

// ============================================================================
// Paper Trading Mode Implementation
// ============================================================================

/**
 * @brief Paper trading mode implementation
 *
 * Simulates order execution using real market data.
 * Features:
 * - Market orders: immediate fill at current price with slippage
 * - Limit orders: fill when price crosses
 * - Partial fill simulation
 * - Realistic fee deduction
 * - Position tracking
 */
class PaperTradingMode : public TradingMode {
public:
    PaperTradingMode();
    ~PaperTradingMode() override;

    // Non-copyable
    PaperTradingMode(const PaperTradingMode&) = delete;
    PaperTradingMode& operator=(const PaperTradingMode&) = delete;

    // ========================================================================
    // Lifecycle Management
    // ========================================================================

    bool initialize(const TradingModeConfig& config) override;
    bool start() override;
    void stop() override;
    [[nodiscard]] bool is_running() const override;
    [[nodiscard]] core::TradingMode mode() const override;

    // ========================================================================
    // Order Management
    // ========================================================================

    OrderResponse submit_order(const OrderRequest& request) override;
    CancelResponse cancel_order(const CancelRequest& request) override;
    uint32_t cancel_all_orders(core::Exchange exchange,
                               const core::Symbol& symbol = core::Symbol{}) override;
    OrderResponse modify_order(core::OrderId original_order_id,
                              const OrderRequest& new_request) override;

    // ========================================================================
    // Position Management
    // ========================================================================

    [[nodiscard]] std::optional<Position> get_position(
        core::Exchange exchange, const core::Symbol& symbol) const override;
    [[nodiscard]] std::vector<Position> get_all_positions(
        core::Exchange exchange = core::Exchange::Unknown) const override;
    OrderResponse close_position(core::Exchange exchange,
                                const core::Symbol& symbol) override;
    uint32_t close_all_positions(
        core::Exchange exchange = core::Exchange::Unknown) override;

    // ========================================================================
    // Balance Management
    // ========================================================================

    [[nodiscard]] std::optional<Balance> get_balance(
        core::Exchange exchange, const std::string& asset) const override;
    [[nodiscard]] std::vector<Balance> get_all_balances(
        core::Exchange exchange = core::Exchange::Unknown) const override;
    [[nodiscard]] core::Price get_total_equity(
        core::Exchange exchange = core::Exchange::Unknown) const override;
    [[nodiscard]] std::optional<AccountInfo> get_account_info(
        core::Exchange exchange) const override;

    // ========================================================================
    // Market Data Integration
    // ========================================================================

    void update_market_price(core::Exchange exchange,
                            const core::Symbol& symbol,
                            core::Price bid,
                            core::Price ask,
                            core::Price last_price,
                            core::Timestamp timestamp) override;

    // ========================================================================
    // Callback Registration
    // ========================================================================

    void set_on_order_response(OnOrderResponseCallback callback) override;
    void set_on_execution(OnExecutionCallback callback) override;
    void set_on_cancel_response(OnCancelResponseCallback callback) override;
    void set_on_position_update(OnPositionUpdateCallback callback) override;
    void set_on_balance_update(OnBalanceUpdateCallback callback) override;
    void set_on_error(OnErrorCallback callback) override;

    // ========================================================================
    // Statistics and Diagnostics
    // ========================================================================

    [[nodiscard]] const TradingModeConfig& config() const override;
    [[nodiscard]] uint64_t orders_submitted() const override;
    [[nodiscard]] uint64_t orders_filled() const override;
    [[nodiscard]] uint64_t orders_cancelled() const override;
    [[nodiscard]] uint64_t orders_rejected() const override;
    [[nodiscard]] core::Price total_volume() const override;
    [[nodiscard]] core::Price total_fees() const override;
    [[nodiscard]] core::Price realized_pnl() const override;
    void reset_statistics() override;

    // ========================================================================
    // Paper Trading Specific
    // ========================================================================

    /**
     * @brief Set initial balance for an asset
     * @param exchange Exchange to set balance on
     * @param asset Asset symbol
     * @param amount Balance amount
     */
    void set_balance(core::Exchange exchange, const std::string& asset,
                    core::Price amount);

    /**
     * @brief Reset paper trading to initial state
     */
    void reset();

    /**
     * @brief Get all active orders
     */
    [[nodiscard]] std::vector<SimulatedOrder> get_active_orders() const;

    /**
     * @brief Get order by ID
     */
    [[nodiscard]] std::optional<SimulatedOrder> get_order(
        core::OrderId order_id) const;

private:
    // ========================================================================
    // Internal Methods
    // ========================================================================

    /**
     * @brief Validate an order request
     */
    core::ErrorCode validate_order(const OrderRequest& request);

    /**
     * @brief Check if we have sufficient balance for an order
     */
    bool check_balance(const OrderRequest& request);

    /**
     * @brief Reserve balance for a new order
     */
    void reserve_balance(const OrderRequest& request);

    /**
     * @brief Release reserved balance (on cancel)
     */
    void release_balance(const SimulatedOrder& order);

    /**
     * @brief Execute a market order immediately
     */
    void execute_market_order(SimulatedOrder& order);

    /**
     * @brief Try to fill a limit order based on current market price
     */
    void try_fill_limit_order(SimulatedOrder& order,
                             const MarketPriceSnapshot& price);

    /**
     * @brief Process a fill
     */
    void process_fill(SimulatedOrder& order, core::Price fill_price,
                     core::Quantity fill_quantity);

    /**
     * @brief Calculate slippage for market orders
     */
    core::Price calculate_slippage(const OrderRequest& request,
                                   const MarketPriceSnapshot& price);

    /**
     * @brief Calculate fees for a fill
     */
    core::Price calculate_fee(core::Exchange exchange, core::Price value,
                             bool is_maker);

    /**
     * @brief Update position after a fill
     */
    void update_position(core::Exchange exchange, const core::Symbol& symbol,
                        core::Side side, core::Price price,
                        core::Quantity quantity);

    /**
     * @brief Update balance after a fill
     */
    void update_balance_after_fill(core::Exchange exchange,
                                   const core::Symbol& symbol,
                                   core::Side side, core::Price price,
                                   core::Quantity quantity, core::Price fee);

    /**
     * @brief Get quote asset from symbol (simple heuristic)
     */
    std::string get_quote_asset(const core::Symbol& symbol);

    /**
     * @brief Get base asset from symbol (simple heuristic)
     */
    std::string get_base_asset(const core::Symbol& symbol);

    /**
     * @brief Generate exchange order ID
     */
    core::OrderId generate_exchange_order_id();

    /**
     * @brief Fill simulation thread function
     */
    void fill_simulation_loop();

    /**
     * @brief Emit callbacks safely
     */
    void emit_order_response(const OrderResponse& response);
    void emit_execution(const ExecutionReport& execution);
    void emit_cancel_response(const CancelResponse& response);
    void emit_position_update(const Position& position);
    void emit_balance_update(const Balance& balance);
    void emit_error(core::ErrorCode code, const std::string& message);

    // ========================================================================
    // Member Variables
    // ========================================================================

    // Configuration
    TradingModeConfig config_;

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};

    // Threading
    std::unique_ptr<std::thread> fill_thread_;
    std::condition_variable_any fill_cv_;
    mutable std::shared_mutex mutex_;

    // Order management
    std::unordered_map<uint64_t, SimulatedOrder> orders_;
    std::unordered_map<uint64_t, SimulatedOrder> active_orders_;
    std::atomic<uint64_t> next_exchange_order_id_{1};

    // Position tracking
    std::unordered_map<SymbolKey, Position, SymbolKeyHash> positions_;

    // Balance tracking
    // Key: "exchange:asset" e.g., "BINANCE:USDT"
    std::unordered_map<std::string, Balance> balances_;

    // Market prices for fill simulation
    std::unordered_map<SymbolKey, MarketPriceSnapshot, SymbolKeyHash> market_prices_;

    // Statistics
    std::atomic<uint64_t> stats_orders_submitted_{0};
    std::atomic<uint64_t> stats_orders_filled_{0};
    std::atomic<uint64_t> stats_orders_cancelled_{0};
    std::atomic<uint64_t> stats_orders_rejected_{0};
    std::atomic<uint64_t> stats_total_volume_{0};
    std::atomic<uint64_t> stats_total_fees_{0};
    std::atomic<int64_t> stats_realized_pnl_{0};

    // Random number generator for partial fills and slippage
    mutable std::mt19937 rng_;
    std::uniform_real_distribution<double> partial_fill_dist_{0.0, 1.0};

    // Callbacks
    OnOrderResponseCallback on_order_response_;
    OnExecutionCallback on_execution_;
    OnCancelResponseCallback on_cancel_response_;
    OnPositionUpdateCallback on_position_update_;
    OnBalanceUpdateCallback on_balance_update_;
    OnErrorCallback on_error_;
    mutable std::mutex callback_mutex_;
};

// ============================================================================
// Factory Function
// ============================================================================

/**
 * @brief Create a paper trading mode instance
 */
[[nodiscard]] std::shared_ptr<PaperTradingMode> create_paper_trading_mode();

}  // namespace hft::trading
