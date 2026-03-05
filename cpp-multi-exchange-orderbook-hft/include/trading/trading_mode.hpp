#pragma once

/**
 * @file trading_mode.hpp
 * @brief Abstract interface for trading modes (paper, live, backtest)
 *
 * This file defines the abstract interface that all trading modes must implement.
 * It provides a unified API for order execution, position management, and balance queries.
 */

#include "core/types.hpp"

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace hft::trading {

// ============================================================================
// Forward Declarations
// ============================================================================

class TradingMode;
using TradingModePtr = std::shared_ptr<TradingMode>;

// ============================================================================
// Order Request/Response Types
// ============================================================================

/**
 * @brief Order request structure sent to the trading mode
 */
struct OrderRequest {
    core::OrderId client_order_id;           // Client-assigned order ID
    core::Exchange exchange{core::Exchange::Unknown};
    core::Symbol symbol;
    core::Side side{core::Side::Buy};
    core::OrderType type{core::OrderType::Limit};
    core::Price price;                       // Limit price (0 for market orders)
    core::Quantity quantity;
    core::Price stop_price;                  // For stop orders
    core::Timestamp timestamp;               // Request timestamp
    bool reduce_only{false};                 // Futures: reduce position only
    bool post_only{false};                   // Maker only
    std::string strategy_id;                 // Originating strategy
    uint64_t user_data{0};                   // User-defined data passed through
};

/**
 * @brief Order response structure returned from the trading mode
 */
struct OrderResponse {
    core::OrderId client_order_id;           // Client-assigned order ID
    core::OrderId exchange_order_id;         // Exchange-assigned order ID
    core::Exchange exchange{core::Exchange::Unknown};
    core::Symbol symbol;
    core::OrderStatus status{core::OrderStatus::Pending};
    core::ErrorCode error_code{core::ErrorCode::Success};
    std::string error_message;
    core::Price executed_price;              // Average execution price
    core::Quantity executed_quantity;        // Filled quantity
    core::Quantity remaining_quantity;       // Remaining quantity
    core::Timestamp timestamp;               // Response timestamp
    uint64_t user_data{0};                   // Passed through from request
};

/**
 * @brief Cancel request structure
 */
struct CancelRequest {
    core::OrderId client_order_id;           // Client-assigned order ID
    core::OrderId exchange_order_id;         // Exchange-assigned order ID (if known)
    core::Exchange exchange{core::Exchange::Unknown};
    core::Symbol symbol;
    core::Timestamp timestamp;
};

/**
 * @brief Cancel response structure
 */
struct CancelResponse {
    core::OrderId client_order_id;
    core::OrderId exchange_order_id;
    core::Exchange exchange{core::Exchange::Unknown};
    core::Symbol symbol;
    bool success{false};
    core::ErrorCode error_code{core::ErrorCode::Success};
    std::string error_message;
    core::Timestamp timestamp;
};

/**
 * @brief Execution/fill report
 */
struct ExecutionReport {
    core::OrderId client_order_id;
    core::OrderId exchange_order_id;
    core::TradeId trade_id;
    core::Exchange exchange{core::Exchange::Unknown};
    core::Symbol symbol;
    core::Side side{core::Side::Buy};
    core::OrderStatus status{core::OrderStatus::New};
    core::Price price;                       // Execution price
    core::Quantity quantity;                 // Execution quantity
    core::Quantity cumulative_quantity;      // Total filled so far
    core::Quantity remaining_quantity;       // Remaining to fill
    core::Price commission;                  // Commission amount
    std::string commission_asset;            // Commission asset
    core::Timestamp timestamp;               // Execution timestamp
    bool is_maker{false};                    // Was this a maker trade?
};

// ============================================================================
// Position and Balance Types
// ============================================================================

/**
 * @brief Position information for a symbol
 */
struct Position {
    core::Exchange exchange{core::Exchange::Unknown};
    core::Symbol symbol;
    core::Quantity quantity;                 // Signed quantity (negative = short)
    core::Price avg_entry_price;             // Average entry price
    core::Price unrealized_pnl;              // Unrealized P&L
    core::Price realized_pnl;                // Realized P&L
    core::Price liquidation_price;           // Liquidation price (futures)
    core::Timestamp updated_at;              // Last update timestamp
    bool is_long{true};                      // Position direction

    [[nodiscard]] bool is_flat() const noexcept {
        return quantity.is_zero();
    }

    [[nodiscard]] double signed_quantity() const noexcept {
        return is_long ? quantity.to_double() : -quantity.to_double();
    }
};

/**
 * @brief Balance information for an asset
 */
struct Balance {
    std::string asset;                       // Asset symbol (BTC, USDT, etc.)
    core::Exchange exchange{core::Exchange::Unknown};
    core::Price total;                       // Total balance
    core::Price available;                   // Available for trading
    core::Price locked;                      // In open orders
    core::Timestamp updated_at;              // Last update timestamp
};

/**
 * @brief Account information
 */
struct AccountInfo {
    core::Exchange exchange{core::Exchange::Unknown};
    std::vector<Balance> balances;
    std::vector<Position> positions;
    core::Price total_equity;                // Total account value in USD
    core::Price margin_used;                 // Used margin (futures)
    core::Price margin_available;            // Available margin (futures)
    double margin_level{0.0};                // Margin level percentage
    bool can_trade{true};
    bool can_withdraw{true};
    bool can_deposit{true};
    core::Timestamp updated_at;
};

// ============================================================================
// Trading Mode Configuration
// ============================================================================

/**
 * @brief Fee structure for an exchange
 */
struct FeeStructure {
    double maker_fee{0.001};                 // 0.1% default maker fee
    double taker_fee{0.001};                 // 0.1% default taker fee
    double funding_rate{0.0};                // Futures funding rate
    bool fee_in_quote{true};                 // Fee charged in quote asset
};

/**
 * @brief Trading mode configuration
 */
struct TradingModeConfig {
    core::TradingMode mode{core::TradingMode::Paper};
    std::string name;                        // Configuration name

    // Exchange-specific fee structures
    std::unordered_map<core::Exchange, FeeStructure> fees;

    // Rate limiting
    uint32_t max_orders_per_second{10};
    uint32_t max_orders_per_minute{600};

    // Risk limits
    core::Price max_position_value;          // Maximum position value
    core::Price max_order_value;             // Maximum single order value
    double max_leverage{1.0};                // Maximum leverage (futures)

    // Paper trading specific
    core::Price initial_balance;             // Initial paper balance
    std::string base_currency{"USDT"};       // Base currency for paper trading

    // Simulation parameters
    uint64_t simulated_latency_us{1000};     // Simulated exchange latency
    double partial_fill_probability{0.1};    // Probability of partial fills
    double slippage_bps{1.0};                // Slippage in basis points

    // Live trading specific
    bool enable_trading{false};              // Master enable switch
    bool dry_run{true};                      // Log orders but don't execute
    uint32_t max_retries{3};                 // Max retry attempts
    uint32_t retry_delay_ms{100};            // Delay between retries
};

// ============================================================================
// Callback Types
// ============================================================================

using OnOrderResponseCallback = std::function<void(const OrderResponse&)>;
using OnExecutionCallback = std::function<void(const ExecutionReport&)>;
using OnCancelResponseCallback = std::function<void(const CancelResponse&)>;
using OnPositionUpdateCallback = std::function<void(const Position&)>;
using OnBalanceUpdateCallback = std::function<void(const Balance&)>;
using OnErrorCallback = std::function<void(core::ErrorCode, const std::string&)>;

// ============================================================================
// Trading Mode Interface
// ============================================================================

/**
 * @brief Abstract interface for trading modes
 *
 * This interface defines the contract that all trading modes must implement.
 * Paper trading, live trading, and backtesting all implement this interface.
 */
class TradingMode {
public:
    virtual ~TradingMode() = default;

    // ========================================================================
    // Lifecycle Management
    // ========================================================================

    /**
     * @brief Initialize the trading mode
     * @param config Configuration for the trading mode
     * @return true if initialization successful
     */
    virtual bool initialize(const TradingModeConfig& config) = 0;

    /**
     * @brief Start the trading mode
     * @return true if started successfully
     */
    virtual bool start() = 0;

    /**
     * @brief Stop the trading mode gracefully
     */
    virtual void stop() = 0;

    /**
     * @brief Check if the trading mode is running
     */
    [[nodiscard]] virtual bool is_running() const = 0;

    /**
     * @brief Get the current trading mode type
     */
    [[nodiscard]] virtual core::TradingMode mode() const = 0;

    // ========================================================================
    // Order Management
    // ========================================================================

    /**
     * @brief Submit a new order
     * @param request Order request details
     * @return Initial order response (async results via callback)
     */
    virtual OrderResponse submit_order(const OrderRequest& request) = 0;

    /**
     * @brief Cancel an existing order
     * @param request Cancel request details
     * @return Cancel response (async results via callback)
     */
    virtual CancelResponse cancel_order(const CancelRequest& request) = 0;

    /**
     * @brief Cancel all orders for a symbol
     * @param exchange Target exchange
     * @param symbol Symbol to cancel orders for (empty = all symbols)
     * @return Number of orders cancelled
     */
    virtual uint32_t cancel_all_orders(core::Exchange exchange,
                                       const core::Symbol& symbol = core::Symbol{}) = 0;

    /**
     * @brief Modify an existing order (cancel + replace)
     * @param original_order_id Original order ID to modify
     * @param new_request New order parameters
     * @return Order response for the new order
     */
    virtual OrderResponse modify_order(core::OrderId original_order_id,
                                       const OrderRequest& new_request) = 0;

    // ========================================================================
    // Position Management
    // ========================================================================

    /**
     * @brief Get current position for a symbol
     * @param exchange Target exchange
     * @param symbol Symbol to query
     * @return Position if exists, nullopt otherwise
     */
    [[nodiscard]] virtual std::optional<Position> get_position(
        core::Exchange exchange, const core::Symbol& symbol) const = 0;

    /**
     * @brief Get all positions
     * @param exchange Filter by exchange (Unknown = all exchanges)
     * @return Vector of all positions
     */
    [[nodiscard]] virtual std::vector<Position> get_all_positions(
        core::Exchange exchange = core::Exchange::Unknown) const = 0;

    /**
     * @brief Close a position for a symbol
     * @param exchange Target exchange
     * @param symbol Symbol to close
     * @return Order response for the closing order
     */
    virtual OrderResponse close_position(core::Exchange exchange,
                                        const core::Symbol& symbol) = 0;

    /**
     * @brief Close all positions
     * @param exchange Filter by exchange (Unknown = all exchanges)
     * @return Number of positions closed
     */
    virtual uint32_t close_all_positions(
        core::Exchange exchange = core::Exchange::Unknown) = 0;

    // ========================================================================
    // Balance Management
    // ========================================================================

    /**
     * @brief Get balance for an asset
     * @param exchange Target exchange
     * @param asset Asset symbol
     * @return Balance if exists, nullopt otherwise
     */
    [[nodiscard]] virtual std::optional<Balance> get_balance(
        core::Exchange exchange, const std::string& asset) const = 0;

    /**
     * @brief Get all balances
     * @param exchange Filter by exchange (Unknown = all exchanges)
     * @return Vector of all balances
     */
    [[nodiscard]] virtual std::vector<Balance> get_all_balances(
        core::Exchange exchange = core::Exchange::Unknown) const = 0;

    /**
     * @brief Get total account equity in USD
     * @param exchange Filter by exchange (Unknown = all exchanges)
     * @return Total equity value
     */
    [[nodiscard]] virtual core::Price get_total_equity(
        core::Exchange exchange = core::Exchange::Unknown) const = 0;

    /**
     * @brief Get account information
     * @param exchange Target exchange
     * @return Account info if available
     */
    [[nodiscard]] virtual std::optional<AccountInfo> get_account_info(
        core::Exchange exchange) const = 0;

    // ========================================================================
    // Market Data Integration
    // ========================================================================

    /**
     * @brief Update market price for fill simulation (paper trading)
     * @param exchange Exchange
     * @param symbol Symbol
     * @param bid Current best bid
     * @param ask Current best ask
     * @param last_price Last trade price
     * @param timestamp Update timestamp
     */
    virtual void update_market_price(core::Exchange exchange,
                                    const core::Symbol& symbol,
                                    core::Price bid,
                                    core::Price ask,
                                    core::Price last_price,
                                    core::Timestamp timestamp) = 0;

    // ========================================================================
    // Callback Registration
    // ========================================================================

    /**
     * @brief Set callback for order responses
     */
    virtual void set_on_order_response(OnOrderResponseCallback callback) = 0;

    /**
     * @brief Set callback for execution reports
     */
    virtual void set_on_execution(OnExecutionCallback callback) = 0;

    /**
     * @brief Set callback for cancel responses
     */
    virtual void set_on_cancel_response(OnCancelResponseCallback callback) = 0;

    /**
     * @brief Set callback for position updates
     */
    virtual void set_on_position_update(OnPositionUpdateCallback callback) = 0;

    /**
     * @brief Set callback for balance updates
     */
    virtual void set_on_balance_update(OnBalanceUpdateCallback callback) = 0;

    /**
     * @brief Set callback for errors
     */
    virtual void set_on_error(OnErrorCallback callback) = 0;

    // ========================================================================
    // Statistics and Diagnostics
    // ========================================================================

    /**
     * @brief Get configuration
     */
    [[nodiscard]] virtual const TradingModeConfig& config() const = 0;

    /**
     * @brief Get number of orders submitted
     */
    [[nodiscard]] virtual uint64_t orders_submitted() const = 0;

    /**
     * @brief Get number of orders filled
     */
    [[nodiscard]] virtual uint64_t orders_filled() const = 0;

    /**
     * @brief Get number of orders cancelled
     */
    [[nodiscard]] virtual uint64_t orders_cancelled() const = 0;

    /**
     * @brief Get number of orders rejected
     */
    [[nodiscard]] virtual uint64_t orders_rejected() const = 0;

    /**
     * @brief Get total trading volume
     */
    [[nodiscard]] virtual core::Price total_volume() const = 0;

    /**
     * @brief Get total fees paid
     */
    [[nodiscard]] virtual core::Price total_fees() const = 0;

    /**
     * @brief Get realized P&L
     */
    [[nodiscard]] virtual core::Price realized_pnl() const = 0;

    /**
     * @brief Reset statistics
     */
    virtual void reset_statistics() = 0;
};

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Generate a unique client order ID
 */
[[nodiscard]] inline core::OrderId generate_client_order_id() {
    static std::atomic<uint64_t> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    // Combine timestamp (high 48 bits) with counter (low 16 bits)
    return core::OrderId{(timestamp << 16) | (counter.fetch_add(1) & 0xFFFF)};
}

/**
 * @brief Get current timestamp in nanoseconds
 */
[[nodiscard]] inline core::Timestamp current_timestamp() {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    return core::Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count())};
}

}  // namespace hft::trading
