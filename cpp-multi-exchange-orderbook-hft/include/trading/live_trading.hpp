#pragma once

/**
 * @file live_trading.hpp
 * @brief Live trading mode implementation
 *
 * This file implements real trading on actual exchanges.
 * Includes comprehensive safety checks, error recovery, and position reconciliation.
 */

#include "trading/trading_mode.hpp"

#include <unordered_map>
#include <map>
#include <deque>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace hft::trading {

// ============================================================================
// Forward Declarations
// ============================================================================

class ExchangeConnector;
using ExchangeConnectorPtr = std::shared_ptr<ExchangeConnector>;

// ============================================================================
// Rate Limiter
// ============================================================================

/**
 * @brief Token bucket rate limiter
 */
class RateLimiter {
public:
    RateLimiter(uint32_t tokens_per_second, uint32_t burst_capacity);

    /**
     * @brief Try to acquire tokens
     * @param tokens Number of tokens to acquire
     * @return true if tokens were acquired
     */
    bool try_acquire(uint32_t tokens = 1);

    /**
     * @brief Acquire tokens, blocking if necessary
     * @param tokens Number of tokens to acquire
     * @param timeout_ms Maximum time to wait
     * @return true if tokens were acquired within timeout
     */
    bool acquire(uint32_t tokens = 1, uint32_t timeout_ms = 1000);

    /**
     * @brief Get current available tokens
     */
    [[nodiscard]] uint32_t available_tokens() const;

    /**
     * @brief Reset the rate limiter
     */
    void reset();

private:
    void refill();

    uint32_t tokens_per_second_;
    uint32_t burst_capacity_;
    std::atomic<uint32_t> available_;
    std::chrono::steady_clock::time_point last_refill_;
    mutable std::mutex mutex_;
};

// ============================================================================
// Safety Checks
// ============================================================================

/**
 * @brief Pre-trade safety check result
 */
struct SafetyCheckResult {
    bool passed{true};
    core::ErrorCode error_code{core::ErrorCode::Success};
    std::string error_message;
    std::vector<std::string> warnings;
};

/**
 * @brief Risk parameters for live trading
 */
struct RiskParameters {
    // Position limits
    core::Price max_position_value;           // Max value per position
    core::Price max_total_exposure;           // Max total exposure across all positions
    core::Quantity max_order_quantity;        // Max single order quantity
    core::Price max_order_value;              // Max single order value

    // Order limits
    uint32_t max_open_orders{100};            // Max concurrent open orders
    uint32_t max_orders_per_symbol{10};       // Max orders per symbol

    // Loss limits
    core::Price max_daily_loss;               // Max daily loss before halt
    core::Price max_drawdown;                 // Max drawdown before halt
    double max_loss_percent{5.0};             // Max loss as % of equity

    // Rate limits
    uint32_t max_orders_per_second{10};
    uint32_t max_orders_per_minute{300};
    uint32_t max_cancels_per_second{20};

    // Timing
    uint64_t min_order_interval_us{10000};    // Min interval between orders
    uint64_t stale_price_threshold_ms{5000};  // Max age of market data

    // Operational
    bool require_two_sided_market{true};      // Require both bid and ask
    double max_spread_percent{5.0};           // Max acceptable spread
    bool check_exchange_status{true};         // Check exchange operational
};

/**
 * @brief Order pending confirmation tracking
 */
struct PendingOrder {
    OrderRequest request;
    core::OrderId exchange_order_id;
    core::Timestamp sent_at;
    uint32_t retry_count{0};
    bool confirmed{false};
    bool timed_out{false};
};

// ============================================================================
// Exchange Connector Interface
// ============================================================================

/**
 * @brief Abstract interface for exchange connectivity
 *
 * This interface defines the contract for exchange connectors.
 * Each exchange (Binance, Coinbase, etc.) implements this interface.
 */
class ExchangeConnector {
public:
    virtual ~ExchangeConnector() = default;

    /**
     * @brief Initialize the connector
     */
    virtual bool initialize() = 0;

    /**
     * @brief Connect to the exchange
     */
    virtual bool connect() = 0;

    /**
     * @brief Disconnect from the exchange
     */
    virtual void disconnect() = 0;

    /**
     * @brief Check if connected
     */
    [[nodiscard]] virtual bool is_connected() const = 0;

    /**
     * @brief Get the exchange type
     */
    [[nodiscard]] virtual core::Exchange exchange() const = 0;

    /**
     * @brief Submit an order to the exchange
     */
    virtual OrderResponse submit_order(const OrderRequest& request) = 0;

    /**
     * @brief Cancel an order on the exchange
     */
    virtual CancelResponse cancel_order(const CancelRequest& request) = 0;

    /**
     * @brief Get account balances
     */
    virtual std::vector<Balance> get_balances() = 0;

    /**
     * @brief Get open positions
     */
    virtual std::vector<Position> get_positions() = 0;

    /**
     * @brief Get open orders
     */
    virtual std::vector<OrderResponse> get_open_orders() = 0;

    /**
     * @brief Synchronize local state with exchange
     */
    virtual bool synchronize() = 0;

    /**
     * @brief Check exchange status/health
     */
    [[nodiscard]] virtual bool is_exchange_healthy() const = 0;

    /**
     * @brief Set execution callback
     */
    virtual void set_on_execution(OnExecutionCallback callback) = 0;

    /**
     * @brief Set order update callback
     */
    virtual void set_on_order_update(OnOrderResponseCallback callback) = 0;
};

// ============================================================================
// Live Trading Mode Implementation
// ============================================================================

/**
 * @brief Live trading mode implementation
 *
 * Executes real trades on actual exchanges.
 * Features:
 * - Real exchange connectivity
 * - Pre-trade risk checks
 * - Rate limiting
 * - Position reconciliation
 * - Error recovery and retry logic
 * - Balance synchronization
 */
class LiveTradingMode : public TradingMode {
public:
    LiveTradingMode();
    ~LiveTradingMode() override;

    // Non-copyable
    LiveTradingMode(const LiveTradingMode&) = delete;
    LiveTradingMode& operator=(const LiveTradingMode&) = delete;

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
    // Live Trading Specific
    // ========================================================================

    /**
     * @brief Register an exchange connector
     */
    void register_connector(ExchangeConnectorPtr connector);

    /**
     * @brief Get connector for exchange
     */
    [[nodiscard]] ExchangeConnectorPtr get_connector(core::Exchange exchange) const;

    /**
     * @brief Set risk parameters
     */
    void set_risk_parameters(const RiskParameters& params);

    /**
     * @brief Get risk parameters
     */
    [[nodiscard]] const RiskParameters& risk_parameters() const;

    /**
     * @brief Perform pre-trade safety check
     */
    [[nodiscard]] SafetyCheckResult perform_safety_check(
        const OrderRequest& request) const;

    /**
     * @brief Synchronize state with all exchanges
     */
    bool synchronize_all();

    /**
     * @brief Enable/disable trading
     */
    void set_trading_enabled(bool enabled);

    /**
     * @brief Check if trading is enabled
     */
    [[nodiscard]] bool is_trading_enabled() const;

    /**
     * @brief Emergency stop - cancel all orders and close all positions
     */
    void emergency_stop();

    /**
     * @brief Get pending orders awaiting confirmation
     */
    [[nodiscard]] std::vector<PendingOrder> get_pending_orders() const;

    /**
     * @brief Get daily P&L
     */
    [[nodiscard]] core::Price daily_pnl() const;

    /**
     * @brief Reset daily P&L tracking (call at start of trading day)
     */
    void reset_daily_pnl();

    /**
     * @brief Check if trading is halted due to risk limits
     */
    [[nodiscard]] bool is_halted() const;

    /**
     * @brief Get halt reason
     */
    [[nodiscard]] std::string halt_reason() const;

    /**
     * @brief Clear halt and resume trading
     */
    void clear_halt();

private:
    // ========================================================================
    // Internal Methods
    // ========================================================================

    /**
     * @brief Validate order request
     */
    core::ErrorCode validate_order(const OrderRequest& request);

    /**
     * @brief Check rate limits
     */
    bool check_rate_limit(core::Exchange exchange);

    /**
     * @brief Check balance sufficiency
     */
    bool check_balance(const OrderRequest& request);

    /**
     * @brief Check position limits
     */
    bool check_position_limits(const OrderRequest& request);

    /**
     * @brief Check if market data is fresh
     */
    bool check_market_data_freshness(core::Exchange exchange,
                                     const core::Symbol& symbol);

    /**
     * @brief Check exchange health
     */
    bool check_exchange_health(core::Exchange exchange);

    /**
     * @brief Execute order with retry logic
     */
    OrderResponse execute_with_retry(ExchangeConnectorPtr connector,
                                     const OrderRequest& request);

    /**
     * @brief Handle execution report from exchange
     */
    void handle_execution(const ExecutionReport& execution);

    /**
     * @brief Handle order update from exchange
     */
    void handle_order_update(const OrderResponse& update);

    /**
     * @brief Update position from execution
     */
    void update_position_from_execution(const ExecutionReport& execution);

    /**
     * @brief Update balance from execution
     */
    void update_balance_from_execution(const ExecutionReport& execution);

    /**
     * @brief Check and trigger risk halt if needed
     */
    void check_risk_limits();

    /**
     * @brief Order timeout check thread
     */
    void timeout_check_loop();

    /**
     * @brief Synchronization thread
     */
    void sync_loop();

    /**
     * @brief Emit callbacks safely
     */
    void emit_order_response(const OrderResponse& response);
    void emit_execution(const ExecutionReport& execution);
    void emit_cancel_response(const CancelResponse& response);
    void emit_position_update(const Position& position);
    void emit_balance_update(const Balance& balance);
    void emit_error(core::ErrorCode code, const std::string& message);

    /**
     * @brief Get quote and base assets from symbol
     */
    std::string get_quote_asset(const core::Symbol& symbol);
    std::string get_base_asset(const core::Symbol& symbol);

    // ========================================================================
    // Member Variables
    // ========================================================================

    // Configuration
    TradingModeConfig config_;
    RiskParameters risk_params_;

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> trading_enabled_{false};
    std::atomic<bool> halted_{false};
    std::string halt_reason_;

    // Threading
    std::unique_ptr<std::thread> timeout_thread_;
    std::unique_ptr<std::thread> sync_thread_;
    std::condition_variable_any shutdown_cv_;
    mutable std::shared_mutex mutex_;

    // Exchange connectors
    std::unordered_map<core::Exchange, ExchangeConnectorPtr> connectors_;

    // Rate limiters per exchange
    std::unordered_map<core::Exchange, std::unique_ptr<RateLimiter>> rate_limiters_;
    std::unordered_map<core::Exchange, std::unique_ptr<RateLimiter>> cancel_limiters_;

    // Order tracking
    std::unordered_map<uint64_t, PendingOrder> pending_orders_;
    std::unordered_map<uint64_t, OrderResponse> confirmed_orders_;
    std::atomic<core::Timestamp> last_order_time_{core::Timestamp{0}};

    // Position tracking
    std::unordered_map<std::string, Position> positions_;  // Key: "exchange:symbol"

    // Balance tracking
    std::unordered_map<std::string, Balance> balances_;    // Key: "exchange:asset"

    // Market prices
    struct MarketPrice {
        core::Price bid;
        core::Price ask;
        core::Price last;
        core::Timestamp timestamp;
    };
    std::unordered_map<std::string, MarketPrice> market_prices_;  // Key: "exchange:symbol"

    // Statistics
    std::atomic<uint64_t> stats_orders_submitted_{0};
    std::atomic<uint64_t> stats_orders_filled_{0};
    std::atomic<uint64_t> stats_orders_cancelled_{0};
    std::atomic<uint64_t> stats_orders_rejected_{0};
    std::atomic<uint64_t> stats_total_volume_{0};
    std::atomic<uint64_t> stats_total_fees_{0};
    std::atomic<int64_t> stats_realized_pnl_{0};
    std::atomic<int64_t> stats_daily_pnl_{0};
    std::atomic<int64_t> stats_peak_equity_{0};

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
// Factory Functions
// ============================================================================

/**
 * @brief Create a live trading mode instance
 */
[[nodiscard]] std::shared_ptr<LiveTradingMode> create_live_trading_mode();

/**
 * @brief Create default risk parameters
 */
[[nodiscard]] RiskParameters create_default_risk_parameters();

}  // namespace hft::trading
