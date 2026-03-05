#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hft {
namespace strategies {

// Forward declarations
struct MarketData;
struct OrderUpdate;
struct Trade;
struct Signal;

// ============================================================================
// Enums and Type Definitions
// ============================================================================

enum class SignalType : uint8_t {
    NONE = 0,
    BUY = 1,
    SELL = 2,
    HOLD = 3,
    CLOSE_LONG = 4,
    CLOSE_SHORT = 5,
    REDUCE_POSITION = 6
};

enum class StrategyState : uint8_t {
    UNINITIALIZED = 0,
    INITIALIZED = 1,
    RUNNING = 2,
    PAUSED = 3,
    STOPPED = 4,
    ERROR = 5
};

enum class OrderSide : uint8_t {
    BUY = 0,
    SELL = 1
};

enum class OrderStatus : uint8_t {
    PENDING = 0,
    OPEN = 1,
    PARTIALLY_FILLED = 2,
    FILLED = 3,
    CANCELLED = 4,
    REJECTED = 5,
    EXPIRED = 6
};

enum class ExecutionMode : uint8_t {
    LIVE = 0,
    BACKTEST = 1,
    PAPER = 2
};

using Timestamp = std::chrono::nanoseconds;
using Price = double;
using Quantity = double;
using ExchangeId = uint8_t;
using SymbolId = uint32_t;

// ============================================================================
// Market Data Structures
// ============================================================================

struct PriceLevel {
    Price price{0.0};
    Quantity quantity{0.0};
    uint32_t order_count{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return price > 0.0 && quantity > 0.0;
    }
};

struct OrderBookSnapshot {
    static constexpr size_t MAX_LEVELS = 20;

    ExchangeId exchange_id{0};
    SymbolId symbol_id{0};
    Timestamp timestamp{0};
    uint64_t sequence{0};

    std::array<PriceLevel, MAX_LEVELS> bids{};
    std::array<PriceLevel, MAX_LEVELS> asks{};
    size_t bid_levels{0};
    size_t ask_levels{0};

    [[nodiscard]] Price best_bid() const noexcept {
        return bid_levels > 0 ? bids[0].price : 0.0;
    }

    [[nodiscard]] Price best_ask() const noexcept {
        return ask_levels > 0 ? asks[0].price : 0.0;
    }

    [[nodiscard]] Price mid_price() const noexcept {
        const auto bb = best_bid();
        const auto ba = best_ask();
        return (bb > 0 && ba > 0) ? (bb + ba) / 2.0 : 0.0;
    }

    [[nodiscard]] Price spread() const noexcept {
        return best_ask() - best_bid();
    }

    [[nodiscard]] double spread_bps() const noexcept {
        const auto mid = mid_price();
        return mid > 0 ? (spread() / mid) * 10000.0 : 0.0;
    }
};

struct MarketData {
    ExchangeId exchange_id{0};
    SymbolId symbol_id{0};
    std::string symbol;
    std::string exchange;

    Timestamp timestamp{0};
    Timestamp exchange_timestamp{0};
    Timestamp local_timestamp{0};

    Price last_price{0.0};
    Price bid_price{0.0};
    Price ask_price{0.0};
    Quantity bid_size{0.0};
    Quantity ask_size{0.0};
    Quantity last_size{0.0};
    Quantity volume_24h{0.0};

    OrderBookSnapshot orderbook;

    [[nodiscard]] Price mid_price() const noexcept {
        return (bid_price > 0 && ask_price > 0) ? (bid_price + ask_price) / 2.0 : last_price;
    }

    [[nodiscard]] Price spread() const noexcept {
        return ask_price - bid_price;
    }

    [[nodiscard]] double spread_bps() const noexcept {
        const auto mid = mid_price();
        return mid > 0 ? (spread() / mid) * 10000.0 : 0.0;
    }

    [[nodiscard]] int64_t latency_ns() const noexcept {
        return (local_timestamp - exchange_timestamp).count();
    }
};

// ============================================================================
// Order and Trade Structures
// ============================================================================

struct OrderUpdate {
    uint64_t order_id{0};
    uint64_t client_order_id{0};
    ExchangeId exchange_id{0};
    SymbolId symbol_id{0};
    std::string symbol;
    std::string exchange;

    OrderSide side{OrderSide::BUY};
    OrderStatus status{OrderStatus::PENDING};

    Price price{0.0};
    Quantity quantity{0.0};
    Quantity filled_quantity{0.0};
    Quantity remaining_quantity{0.0};
    Price average_fill_price{0.0};

    Timestamp timestamp{0};
    Timestamp update_timestamp{0};

    std::string error_message;

    [[nodiscard]] bool is_active() const noexcept {
        return status == OrderStatus::OPEN || status == OrderStatus::PARTIALLY_FILLED;
    }

    [[nodiscard]] bool is_terminal() const noexcept {
        return status == OrderStatus::FILLED || status == OrderStatus::CANCELLED ||
               status == OrderStatus::REJECTED || status == OrderStatus::EXPIRED;
    }

    [[nodiscard]] double fill_ratio() const noexcept {
        return quantity > 0 ? filled_quantity / quantity : 0.0;
    }
};

struct Trade {
    uint64_t trade_id{0};
    uint64_t order_id{0};
    ExchangeId exchange_id{0};
    SymbolId symbol_id{0};
    std::string symbol;
    std::string exchange;

    OrderSide side{OrderSide::BUY};
    Price price{0.0};
    Quantity quantity{0.0};
    Price commission{0.0};
    std::string commission_asset;

    Timestamp timestamp{0};
    bool is_maker{false};

    [[nodiscard]] double notional() const noexcept {
        return price * quantity;
    }
};

// ============================================================================
// Signal Structure
// ============================================================================

struct Signal {
    SignalType type{SignalType::NONE};
    std::string symbol;
    std::string exchange;
    ExchangeId exchange_id{0};
    SymbolId symbol_id{0};

    Price target_price{0.0};
    Quantity target_quantity{0.0};
    Price stop_loss{0.0};
    Price take_profit{0.0};

    double confidence{0.0};       // 0.0 to 1.0
    double urgency{0.0};          // 0.0 to 1.0, higher = more urgent
    int64_t timeout_us{0};        // Signal validity timeout

    Timestamp timestamp{0};
    std::string reason;
    std::string strategy_id;

    // Additional metadata for multi-leg strategies
    std::vector<std::pair<std::string, OrderSide>> legs;  // (symbol, side) pairs
    std::vector<Price> leg_prices;
    std::vector<Quantity> leg_quantities;

    [[nodiscard]] bool is_actionable() const noexcept {
        return type != SignalType::NONE && type != SignalType::HOLD &&
               confidence > 0.0 && target_quantity > 0.0;
    }

    [[nodiscard]] bool is_buy_signal() const noexcept {
        return type == SignalType::BUY;
    }

    [[nodiscard]] bool is_sell_signal() const noexcept {
        return type == SignalType::SELL || type == SignalType::CLOSE_LONG;
    }
};

// ============================================================================
// Position and Risk Structures
// ============================================================================

struct Position {
    std::string symbol;
    std::string exchange;
    ExchangeId exchange_id{0};
    SymbolId symbol_id{0};

    Quantity quantity{0.0};           // Positive = long, negative = short
    Price average_entry_price{0.0};
    Price unrealized_pnl{0.0};
    Price realized_pnl{0.0};
    Price mark_price{0.0};

    Timestamp open_timestamp{0};
    Timestamp update_timestamp{0};

    [[nodiscard]] bool is_long() const noexcept { return quantity > 0; }
    [[nodiscard]] bool is_short() const noexcept { return quantity < 0; }
    [[nodiscard]] bool is_flat() const noexcept { return std::abs(quantity) < 1e-10; }
    [[nodiscard]] double notional() const noexcept { return std::abs(quantity * mark_price); }
};

struct RiskMetrics {
    double total_exposure{0.0};
    double net_exposure{0.0};
    double max_drawdown{0.0};
    double current_drawdown{0.0};
    double sharpe_ratio{0.0};
    double win_rate{0.0};
    double profit_factor{0.0};
    double total_pnl{0.0};
    double daily_pnl{0.0};
    int total_trades{0};
    int winning_trades{0};
    int losing_trades{0};

    [[nodiscard]] bool is_within_limits(double max_exposure, double max_drawdown_limit) const noexcept {
        return total_exposure <= max_exposure && current_drawdown <= max_drawdown_limit;
    }
};

// ============================================================================
// Strategy Configuration
// ============================================================================

struct StrategyConfig {
    std::string strategy_id;
    std::string strategy_name;
    ExecutionMode execution_mode{ExecutionMode::PAPER};

    // Risk parameters
    double max_position_value{10000.0};
    double max_position_quantity{1.0};
    double max_daily_loss{1000.0};
    double max_drawdown{0.05};           // 5%
    double position_size_pct{0.01};      // 1% of capital per trade

    // Execution parameters
    int64_t signal_timeout_us{100000};   // 100ms
    int64_t order_timeout_us{5000000};   // 5s
    bool allow_partial_fills{true};

    // Symbol configuration
    std::vector<std::string> symbols;
    std::vector<std::string> exchanges;

    // Custom parameters stored as key-value pairs
    std::unordered_map<std::string, double> numeric_params;
    std::unordered_map<std::string, std::string> string_params;
    std::unordered_map<std::string, bool> bool_params;

    template<typename T>
    [[nodiscard]] T get_param(const std::string& key, T default_value) const;
};

template<>
[[nodiscard]] inline double StrategyConfig::get_param(const std::string& key, double default_value) const {
    auto it = numeric_params.find(key);
    return it != numeric_params.end() ? it->second : default_value;
}

template<>
[[nodiscard]] inline std::string StrategyConfig::get_param(const std::string& key, std::string default_value) const {
    auto it = string_params.find(key);
    return it != string_params.end() ? it->second : default_value;
}

template<>
[[nodiscard]] inline bool StrategyConfig::get_param(const std::string& key, bool default_value) const {
    auto it = bool_params.find(key);
    return it != bool_params.end() ? it->second : default_value;
}

// ============================================================================
// Abstract Strategy Base Class
// ============================================================================

class StrategyBase {
public:
    explicit StrategyBase(StrategyConfig config)
        : config_(std::move(config))
        , state_(StrategyState::UNINITIALIZED)
        , is_backtest_mode_(config_.execution_mode == ExecutionMode::BACKTEST)
    {}

    virtual ~StrategyBase() = default;

    // Non-copyable, moveable
    StrategyBase(const StrategyBase&) = delete;
    StrategyBase& operator=(const StrategyBase&) = delete;
    StrategyBase(StrategyBase&&) = default;
    StrategyBase& operator=(StrategyBase&&) = default;

    // ========================================================================
    // Lifecycle Methods
    // ========================================================================

    /**
     * Initialize the strategy with necessary resources
     * @return true if initialization successful
     */
    virtual bool initialize() {
        if (state_ != StrategyState::UNINITIALIZED) {
            return false;
        }
        state_ = StrategyState::INITIALIZED;
        return true;
    }

    /**
     * Start the strategy
     * @return true if start successful
     */
    virtual bool start() {
        if (state_ != StrategyState::INITIALIZED && state_ != StrategyState::PAUSED) {
            return false;
        }
        state_ = StrategyState::RUNNING;
        return true;
    }

    /**
     * Pause the strategy (stops generating signals but maintains state)
     */
    virtual void pause() {
        if (state_ == StrategyState::RUNNING) {
            state_ = StrategyState::PAUSED;
        }
    }

    /**
     * Stop the strategy completely
     */
    virtual void stop() {
        state_ = StrategyState::STOPPED;
        on_stop();
    }

    /**
     * Reset strategy to initial state
     */
    virtual void reset() {
        positions_.clear();
        pending_signals_.clear();
        risk_metrics_ = RiskMetrics{};
        state_ = StrategyState::INITIALIZED;
        on_reset();
    }

    // ========================================================================
    // Event Callbacks (Pure Virtual - Must Implement)
    // ========================================================================

    /**
     * Called when new market data arrives
     * This is the primary entry point for strategy logic
     */
    virtual void on_market_data(const MarketData& data) = 0;

    /**
     * Called when an order status changes
     */
    virtual void on_order_update(const OrderUpdate& update) = 0;

    /**
     * Called when a trade executes
     */
    virtual void on_trade(const Trade& trade) = 0;

    // ========================================================================
    // Signal Generation
    // ========================================================================

    /**
     * Get pending signals that need execution
     * @return vector of signals to be executed
     */
    [[nodiscard]] virtual std::vector<Signal> get_signals() {
        auto signals = std::move(pending_signals_);
        pending_signals_.clear();
        return signals;
    }

    /**
     * Check if strategy has pending signals
     */
    [[nodiscard]] bool has_signals() const noexcept {
        return !pending_signals_.empty();
    }

    // ========================================================================
    // State and Configuration Access
    // ========================================================================

    [[nodiscard]] StrategyState state() const noexcept { return state_; }
    [[nodiscard]] bool is_running() const noexcept { return state_ == StrategyState::RUNNING; }
    [[nodiscard]] bool is_backtest_mode() const noexcept { return is_backtest_mode_; }
    [[nodiscard]] const StrategyConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::string& strategy_id() const noexcept { return config_.strategy_id; }
    [[nodiscard]] const std::string& strategy_name() const noexcept { return config_.strategy_name; }

    // ========================================================================
    // Position and Risk Management
    // ========================================================================

    [[nodiscard]] const std::unordered_map<std::string, Position>& positions() const noexcept {
        return positions_;
    }

    [[nodiscard]] const RiskMetrics& risk_metrics() const noexcept {
        return risk_metrics_;
    }

    [[nodiscard]] std::optional<Position> get_position(const std::string& symbol) const {
        auto it = positions_.find(symbol);
        if (it != positions_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /**
     * Check if a new position can be opened based on risk limits
     */
    [[nodiscard]] bool can_open_position(double notional_value) const noexcept {
        return risk_metrics_.total_exposure + notional_value <= config_.max_position_value &&
               risk_metrics_.current_drawdown < config_.max_drawdown &&
               risk_metrics_.daily_pnl > -config_.max_daily_loss;
    }

protected:
    // ========================================================================
    // Protected Helper Methods
    // ========================================================================

    /**
     * Emit a trading signal
     */
    void emit_signal(Signal signal) {
        signal.strategy_id = config_.strategy_id;
        signal.timestamp = std::chrono::duration_cast<Timestamp>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        );
        if (signal.timeout_us == 0) {
            signal.timeout_us = config_.signal_timeout_us;
        }
        pending_signals_.push_back(std::move(signal));
    }

    /**
     * Update position after trade
     */
    void update_position(const Trade& trade) {
        auto& pos = positions_[trade.symbol];
        pos.symbol = trade.symbol;
        pos.exchange = trade.exchange;
        pos.exchange_id = trade.exchange_id;
        pos.symbol_id = trade.symbol_id;

        double old_quantity = pos.quantity;
        double trade_qty = (trade.side == OrderSide::BUY) ? trade.quantity : -trade.quantity;

        // Update average entry price
        if ((old_quantity >= 0 && trade_qty > 0) || (old_quantity <= 0 && trade_qty < 0)) {
            // Adding to position
            double old_notional = std::abs(old_quantity) * pos.average_entry_price;
            double new_notional = std::abs(trade_qty) * trade.price;
            pos.quantity += trade_qty;
            if (std::abs(pos.quantity) > 1e-10) {
                pos.average_entry_price = (old_notional + new_notional) / std::abs(pos.quantity);
            }
        } else {
            // Reducing or flipping position
            double realized = std::min(std::abs(old_quantity), std::abs(trade_qty)) *
                             (trade.price - pos.average_entry_price) * (old_quantity > 0 ? 1 : -1);
            pos.realized_pnl += realized;
            risk_metrics_.total_pnl += realized;
            risk_metrics_.daily_pnl += realized;

            pos.quantity += trade_qty;
            if (std::abs(pos.quantity) > 1e-10 &&
                ((old_quantity > 0 && pos.quantity < 0) || (old_quantity < 0 && pos.quantity > 0))) {
                pos.average_entry_price = trade.price;
            }
        }

        pos.update_timestamp = trade.timestamp;
        if (old_quantity == 0 || (old_quantity * pos.quantity < 0)) {
            pos.open_timestamp = trade.timestamp;
        }

        update_risk_metrics();
    }

    /**
     * Update unrealized PnL for a position
     */
    void update_mark_price(const std::string& symbol, Price mark_price) {
        auto it = positions_.find(symbol);
        if (it != positions_.end()) {
            auto& pos = it->second;
            pos.mark_price = mark_price;
            pos.unrealized_pnl = pos.quantity * (mark_price - pos.average_entry_price);
        }
    }

    /**
     * Calculate total exposure and update risk metrics
     */
    void update_risk_metrics() {
        double total_exposure = 0.0;
        double net_exposure = 0.0;

        for (const auto& [symbol, pos] : positions_) {
            double notional = pos.notional();
            total_exposure += notional;
            net_exposure += pos.quantity > 0 ? notional : -notional;
        }

        risk_metrics_.total_exposure = total_exposure;
        risk_metrics_.net_exposure = net_exposure;

        // Update drawdown
        static double peak_pnl = risk_metrics_.total_pnl;
        peak_pnl = std::max(peak_pnl, risk_metrics_.total_pnl);
        if (peak_pnl > 0) {
            risk_metrics_.current_drawdown = (peak_pnl - risk_metrics_.total_pnl) / peak_pnl;
            risk_metrics_.max_drawdown = std::max(risk_metrics_.max_drawdown, risk_metrics_.current_drawdown);
        }
    }

    /**
     * Check if risk limits are breached
     */
    [[nodiscard]] bool check_risk_limits() const noexcept {
        return risk_metrics_.is_within_limits(config_.max_position_value, config_.max_drawdown) &&
               risk_metrics_.daily_pnl > -config_.max_daily_loss;
    }

    /**
     * Calculate position size based on risk parameters
     */
    [[nodiscard]] Quantity calculate_position_size(Price price, double volatility = 0.0) const {
        double capital = config_.max_position_value;
        double size_pct = config_.position_size_pct;

        // Adjust for volatility if provided
        if (volatility > 0) {
            size_pct = std::min(size_pct, 0.02 / volatility);  // Target 2% max loss
        }

        double notional = capital * size_pct;
        double max_qty = config_.max_position_quantity;

        return std::min(notional / price, max_qty);
    }

    /**
     * Get current timestamp (handles backtest vs live mode)
     */
    [[nodiscard]] Timestamp current_timestamp() const noexcept {
        if (is_backtest_mode_) {
            return backtest_timestamp_;
        }
        return std::chrono::duration_cast<Timestamp>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        );
    }

    /**
     * Set backtest timestamp (for backtest mode only)
     */
    void set_backtest_timestamp(Timestamp ts) {
        if (is_backtest_mode_) {
            backtest_timestamp_ = ts;
        }
    }

    // ========================================================================
    // Optional Override Hooks
    // ========================================================================

    virtual void on_stop() {}
    virtual void on_reset() {}

protected:
    StrategyConfig config_;
    StrategyState state_;
    bool is_backtest_mode_;
    Timestamp backtest_timestamp_{0};

    std::unordered_map<std::string, Position> positions_;
    std::vector<Signal> pending_signals_;
    RiskMetrics risk_metrics_;
};

// ============================================================================
// Strategy Factory Interface
// ============================================================================

class IStrategyFactory {
public:
    virtual ~IStrategyFactory() = default;
    [[nodiscard]] virtual std::unique_ptr<StrategyBase> create(const StrategyConfig& config) = 0;
    [[nodiscard]] virtual std::string_view strategy_type() const noexcept = 0;
};

// ============================================================================
// Strategy Registry (Singleton)
// ============================================================================

class StrategyRegistry {
public:
    static StrategyRegistry& instance() {
        static StrategyRegistry registry;
        return registry;
    }

    void register_factory(std::string_view type, std::unique_ptr<IStrategyFactory> factory) {
        factories_[std::string(type)] = std::move(factory);
    }

    [[nodiscard]] std::unique_ptr<StrategyBase> create(const std::string& type, const StrategyConfig& config) {
        auto it = factories_.find(type);
        if (it != factories_.end()) {
            return it->second->create(config);
        }
        return nullptr;
    }

    [[nodiscard]] std::vector<std::string> available_strategies() const {
        std::vector<std::string> types;
        types.reserve(factories_.size());
        for (const auto& [type, _] : factories_) {
            types.push_back(type);
        }
        return types;
    }

private:
    StrategyRegistry() = default;
    std::unordered_map<std::string, std::unique_ptr<IStrategyFactory>> factories_;
};

}  // namespace strategies
}  // namespace hft
