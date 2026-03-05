#include "trading/paper_trading.hpp"

#include <algorithm>
#include <sstream>
#include <cmath>
#include <iostream>

namespace hft::trading {

// ============================================================================
// Constructor / Destructor
// ============================================================================

PaperTradingMode::PaperTradingMode()
    : rng_(std::random_device{}()) {
}

PaperTradingMode::~PaperTradingMode() {
    stop();
}

// ============================================================================
// Lifecycle Management
// ============================================================================

bool PaperTradingMode::initialize(const TradingModeConfig& config) {
    std::unique_lock lock(mutex_);

    if (initialized_) {
        return true;
    }

    config_ = config;

    // Set default fees if not provided
    if (config_.fees.empty()) {
        FeeStructure default_fees;
        default_fees.maker_fee = 0.001;  // 0.1%
        default_fees.taker_fee = 0.001;
        config_.fees[core::Exchange::Binance] = default_fees;
        config_.fees[core::Exchange::Coinbase] = default_fees;
        config_.fees[core::Exchange::Kraken] = default_fees;
    }

    // Initialize base currency balance
    if (config_.initial_balance.value > 0) {
        Balance initial_bal;
        initial_bal.asset = config_.base_currency;
        initial_bal.exchange = core::Exchange::Binance;  // Default exchange
        initial_bal.total = config_.initial_balance;
        initial_bal.available = config_.initial_balance;
        initial_bal.locked = core::Price{0};
        initial_bal.updated_at = current_timestamp();

        std::string key = std::string(core::exchange_to_string(core::Exchange::Binance)) +
                         ":" + config_.base_currency;
        balances_[key] = initial_bal;
    }

    initialized_ = true;
    return true;
}

bool PaperTradingMode::start() {
    if (!initialized_) {
        emit_error(core::ErrorCode::InvalidConfiguration,
                   "Paper trading mode not initialized");
        return false;
    }

    if (running_.exchange(true)) {
        return true;  // Already running
    }

    // Start fill simulation thread
    fill_thread_ = std::make_unique<std::thread>([this]() {
        fill_simulation_loop();
    });

    return true;
}

void PaperTradingMode::stop() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    // Wake up fill thread
    fill_cv_.notify_all();

    // Wait for thread to finish
    if (fill_thread_ && fill_thread_->joinable()) {
        fill_thread_->join();
    }
    fill_thread_.reset();
}

bool PaperTradingMode::is_running() const {
    return running_.load();
}

core::TradingMode PaperTradingMode::mode() const {
    return core::TradingMode::Paper;
}

// ============================================================================
// Order Management
// ============================================================================

OrderResponse PaperTradingMode::submit_order(const OrderRequest& request) {
    OrderResponse response;
    response.client_order_id = request.client_order_id;
    response.exchange = request.exchange;
    response.symbol = request.symbol;
    response.timestamp = current_timestamp();
    response.user_data = request.user_data;

    // Validate order
    auto error = validate_order(request);
    if (error != core::ErrorCode::Success) {
        response.status = core::OrderStatus::Rejected;
        response.error_code = error;
        response.error_message = std::string(core::error_code_to_string(error));
        stats_orders_rejected_++;
        emit_order_response(response);
        return response;
    }

    // Check balance
    if (!check_balance(request)) {
        response.status = core::OrderStatus::Rejected;
        response.error_code = core::ErrorCode::InsufficientBalance;
        response.error_message = "Insufficient balance for order";
        stats_orders_rejected_++;
        emit_order_response(response);
        return response;
    }

    // Create simulated order
    SimulatedOrder order;
    order.request = request;
    order.exchange_order_id = generate_exchange_order_id();
    order.status = core::OrderStatus::New;
    order.filled_quantity = core::Quantity{0};
    order.avg_fill_price = core::Price{0};
    order.created_at = current_timestamp();
    order.updated_at = order.created_at;

    // Reserve balance
    reserve_balance(request);

    {
        std::unique_lock lock(mutex_);

        // Add to order maps
        orders_[order.exchange_order_id.value] = order;

        // For market orders, execute immediately
        if (request.type == core::OrderType::Market) {
            execute_market_order(orders_[order.exchange_order_id.value]);
            order = orders_[order.exchange_order_id.value];
        } else {
            // Add to active orders for fill simulation
            active_orders_[order.exchange_order_id.value] = order;
        }
    }

    stats_orders_submitted_++;

    // Prepare response
    response.exchange_order_id = order.exchange_order_id;
    response.status = order.status;
    response.executed_price = order.avg_fill_price;
    response.executed_quantity = order.filled_quantity;
    response.remaining_quantity = core::Quantity{
        request.quantity.value - order.filled_quantity.value};

    emit_order_response(response);

    // Wake up fill thread for limit orders
    if (request.type != core::OrderType::Market) {
        fill_cv_.notify_one();
    }

    return response;
}

CancelResponse PaperTradingMode::cancel_order(const CancelRequest& request) {
    CancelResponse response;
    response.client_order_id = request.client_order_id;
    response.exchange_order_id = request.exchange_order_id;
    response.exchange = request.exchange;
    response.symbol = request.symbol;
    response.timestamp = current_timestamp();

    std::unique_lock lock(mutex_);

    // Find order by exchange order ID or client order ID
    SimulatedOrder* order = nullptr;
    uint64_t order_key = 0;

    if (request.exchange_order_id.is_valid()) {
        auto it = active_orders_.find(request.exchange_order_id.value);
        if (it != active_orders_.end()) {
            order = &it->second;
            order_key = it->first;
        }
    }

    if (!order && request.client_order_id.is_valid()) {
        for (auto& [key, ord] : active_orders_) {
            if (ord.request.client_order_id == request.client_order_id) {
                order = &ord;
                order_key = key;
                break;
            }
        }
    }

    if (!order) {
        response.success = false;
        response.error_code = core::ErrorCode::OrderNotFound;
        response.error_message = "Order not found";
        emit_cancel_response(response);
        return response;
    }

    // Cancel the order
    order->status = core::OrderStatus::Cancelled;
    order->updated_at = current_timestamp();

    // Release reserved balance
    release_balance(*order);

    // Update in main orders map
    orders_[order_key] = *order;

    // Remove from active orders
    active_orders_.erase(order_key);

    response.success = true;
    stats_orders_cancelled_++;

    lock.unlock();
    emit_cancel_response(response);

    return response;
}

uint32_t PaperTradingMode::cancel_all_orders(core::Exchange exchange,
                                             const core::Symbol& symbol) {
    std::unique_lock lock(mutex_);

    std::vector<uint64_t> to_cancel;

    for (auto& [key, order] : active_orders_) {
        bool match = true;

        if (exchange != core::Exchange::Unknown &&
            order.request.exchange != exchange) {
            match = false;
        }

        if (!symbol.empty() && !(order.request.symbol == symbol)) {
            match = false;
        }

        if (match) {
            to_cancel.push_back(key);
        }
    }

    uint32_t cancelled = 0;
    for (uint64_t key : to_cancel) {
        auto& order = active_orders_[key];
        order.status = core::OrderStatus::Cancelled;
        order.updated_at = current_timestamp();
        release_balance(order);
        orders_[key] = order;
        active_orders_.erase(key);
        cancelled++;
        stats_orders_cancelled_++;
    }

    return cancelled;
}

OrderResponse PaperTradingMode::modify_order(core::OrderId original_order_id,
                                             const OrderRequest& new_request) {
    // Cancel the original order
    CancelRequest cancel_req;
    cancel_req.exchange_order_id = original_order_id;
    cancel_req.timestamp = current_timestamp();
    cancel_order(cancel_req);

    // Submit the new order
    return submit_order(new_request);
}

// ============================================================================
// Position Management
// ============================================================================

std::optional<Position> PaperTradingMode::get_position(
    core::Exchange exchange, const core::Symbol& symbol) const {
    std::shared_lock lock(mutex_);

    SymbolKey key{exchange, symbol};
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<Position> PaperTradingMode::get_all_positions(
    core::Exchange exchange) const {
    std::shared_lock lock(mutex_);

    std::vector<Position> result;
    for (const auto& [key, pos] : positions_) {
        if (exchange == core::Exchange::Unknown || key.exchange == exchange) {
            if (!pos.is_flat()) {
                result.push_back(pos);
            }
        }
    }

    return result;
}

OrderResponse PaperTradingMode::close_position(core::Exchange exchange,
                                               const core::Symbol& symbol) {
    auto pos_opt = get_position(exchange, symbol);
    if (!pos_opt || pos_opt->is_flat()) {
        OrderResponse response;
        response.status = core::OrderStatus::Rejected;
        response.error_code = core::ErrorCode::OrderRejected;
        response.error_message = "No position to close";
        return response;
    }

    const auto& pos = *pos_opt;

    // Create market order to close position
    OrderRequest request;
    request.client_order_id = generate_client_order_id();
    request.exchange = exchange;
    request.symbol = symbol;
    request.side = pos.is_long ? core::Side::Sell : core::Side::Buy;
    request.type = core::OrderType::Market;
    request.quantity = pos.quantity;
    request.timestamp = current_timestamp();
    request.reduce_only = true;

    return submit_order(request);
}

uint32_t PaperTradingMode::close_all_positions(core::Exchange exchange) {
    std::vector<std::pair<core::Exchange, core::Symbol>> positions_to_close;

    {
        std::shared_lock lock(mutex_);
        for (const auto& [key, pos] : positions_) {
            if ((exchange == core::Exchange::Unknown || key.exchange == exchange) &&
                !pos.is_flat()) {
                positions_to_close.emplace_back(key.exchange, key.symbol);
            }
        }
    }

    uint32_t closed = 0;
    for (const auto& [ex, sym] : positions_to_close) {
        auto response = close_position(ex, sym);
        if (response.status == core::OrderStatus::Filled ||
            response.status == core::OrderStatus::New) {
            closed++;
        }
    }

    return closed;
}

// ============================================================================
// Balance Management
// ============================================================================

std::optional<Balance> PaperTradingMode::get_balance(
    core::Exchange exchange, const std::string& asset) const {
    std::shared_lock lock(mutex_);

    std::string key = std::string(core::exchange_to_string(exchange)) + ":" + asset;
    auto it = balances_.find(key);
    if (it != balances_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<Balance> PaperTradingMode::get_all_balances(
    core::Exchange exchange) const {
    std::shared_lock lock(mutex_);

    std::vector<Balance> result;
    for (const auto& [key, bal] : balances_) {
        if (exchange == core::Exchange::Unknown || bal.exchange == exchange) {
            if (bal.total.value > 0) {
                result.push_back(bal);
            }
        }
    }

    return result;
}

core::Price PaperTradingMode::get_total_equity(core::Exchange exchange) const {
    std::shared_lock lock(mutex_);

    uint64_t total = 0;

    for (const auto& [key, bal] : balances_) {
        if (exchange == core::Exchange::Unknown || bal.exchange == exchange) {
            // Simple: assume quote currency is equity
            // In real implementation, would convert all assets to base currency
            if (bal.asset == config_.base_currency) {
                total += bal.total.value;
            }
        }
    }

    // Add unrealized P&L from positions
    for (const auto& [key, pos] : positions_) {
        if (exchange == core::Exchange::Unknown || key.exchange == exchange) {
            // This is simplified - would need current price to calculate properly
            total += pos.unrealized_pnl.value;
        }
    }

    return core::Price{total};
}

std::optional<AccountInfo> PaperTradingMode::get_account_info(
    core::Exchange exchange) const {
    AccountInfo info;
    info.exchange = exchange;
    info.balances = get_all_balances(exchange);
    info.positions = get_all_positions(exchange);
    info.total_equity = get_total_equity(exchange);
    info.margin_available = info.total_equity;
    info.margin_used = core::Price{0};
    info.margin_level = 100.0;
    info.can_trade = true;
    info.can_withdraw = true;
    info.can_deposit = true;
    info.updated_at = current_timestamp();

    return info;
}

// ============================================================================
// Market Data Integration
// ============================================================================

void PaperTradingMode::update_market_price(core::Exchange exchange,
                                           const core::Symbol& symbol,
                                           core::Price bid,
                                           core::Price ask,
                                           core::Price last_price,
                                           core::Timestamp timestamp) {
    {
        std::unique_lock lock(mutex_);

        SymbolKey key{exchange, symbol};
        MarketPriceSnapshot& snapshot = market_prices_[key];
        snapshot.bid = bid;
        snapshot.ask = ask;
        snapshot.last_price = last_price;
        snapshot.timestamp = timestamp;
        snapshot.valid = true;

        // Update unrealized P&L for positions
        auto pos_it = positions_.find(key);
        if (pos_it != positions_.end() && !pos_it->second.is_flat()) {
            Position& pos = pos_it->second;
            core::Price current_price = pos.is_long ? bid : ask;

            // Calculate unrealized P&L
            int64_t pnl = 0;
            if (pos.is_long) {
                pnl = static_cast<int64_t>(current_price.value) -
                      static_cast<int64_t>(pos.avg_entry_price.value);
            } else {
                pnl = static_cast<int64_t>(pos.avg_entry_price.value) -
                      static_cast<int64_t>(current_price.value);
            }
            // Scale by quantity
            pnl = (pnl * static_cast<int64_t>(pos.quantity.value)) /
                  static_cast<int64_t>(core::QUANTITY_SCALE);
            pos.unrealized_pnl = core::Price{static_cast<uint64_t>(std::max(0LL, pnl))};
            pos.updated_at = timestamp;
        }
    }

    // Wake up fill thread to check for limit order fills
    fill_cv_.notify_one();
}

// ============================================================================
// Callback Registration
// ============================================================================

void PaperTradingMode::set_on_order_response(OnOrderResponseCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_order_response_ = std::move(callback);
}

void PaperTradingMode::set_on_execution(OnExecutionCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_execution_ = std::move(callback);
}

void PaperTradingMode::set_on_cancel_response(OnCancelResponseCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_cancel_response_ = std::move(callback);
}

void PaperTradingMode::set_on_position_update(OnPositionUpdateCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_position_update_ = std::move(callback);
}

void PaperTradingMode::set_on_balance_update(OnBalanceUpdateCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_balance_update_ = std::move(callback);
}

void PaperTradingMode::set_on_error(OnErrorCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_error_ = std::move(callback);
}

// ============================================================================
// Statistics and Diagnostics
// ============================================================================

const TradingModeConfig& PaperTradingMode::config() const {
    return config_;
}

uint64_t PaperTradingMode::orders_submitted() const {
    return stats_orders_submitted_.load();
}

uint64_t PaperTradingMode::orders_filled() const {
    return stats_orders_filled_.load();
}

uint64_t PaperTradingMode::orders_cancelled() const {
    return stats_orders_cancelled_.load();
}

uint64_t PaperTradingMode::orders_rejected() const {
    return stats_orders_rejected_.load();
}

core::Price PaperTradingMode::total_volume() const {
    return core::Price{stats_total_volume_.load()};
}

core::Price PaperTradingMode::total_fees() const {
    return core::Price{stats_total_fees_.load()};
}

core::Price PaperTradingMode::realized_pnl() const {
    int64_t pnl = stats_realized_pnl_.load();
    return core::Price{static_cast<uint64_t>(std::max(0LL, pnl))};
}

void PaperTradingMode::reset_statistics() {
    stats_orders_submitted_ = 0;
    stats_orders_filled_ = 0;
    stats_orders_cancelled_ = 0;
    stats_orders_rejected_ = 0;
    stats_total_volume_ = 0;
    stats_total_fees_ = 0;
    stats_realized_pnl_ = 0;
}

// ============================================================================
// Paper Trading Specific
// ============================================================================

void PaperTradingMode::set_balance(core::Exchange exchange,
                                   const std::string& asset,
                                   core::Price amount) {
    std::unique_lock lock(mutex_);

    std::string key = std::string(core::exchange_to_string(exchange)) + ":" + asset;
    Balance& bal = balances_[key];
    bal.asset = asset;
    bal.exchange = exchange;
    bal.total = amount;
    bal.available = amount;
    bal.locked = core::Price{0};
    bal.updated_at = current_timestamp();

    lock.unlock();
    emit_balance_update(bal);
}

void PaperTradingMode::reset() {
    std::unique_lock lock(mutex_);

    // Clear all state
    orders_.clear();
    active_orders_.clear();
    positions_.clear();
    balances_.clear();
    market_prices_.clear();

    // Reset statistics
    reset_statistics();

    // Reinitialize base currency balance
    if (config_.initial_balance.value > 0) {
        Balance initial_bal;
        initial_bal.asset = config_.base_currency;
        initial_bal.exchange = core::Exchange::Binance;
        initial_bal.total = config_.initial_balance;
        initial_bal.available = config_.initial_balance;
        initial_bal.locked = core::Price{0};
        initial_bal.updated_at = current_timestamp();

        std::string key = std::string(core::exchange_to_string(core::Exchange::Binance)) +
                         ":" + config_.base_currency;
        balances_[key] = initial_bal;
    }
}

std::vector<SimulatedOrder> PaperTradingMode::get_active_orders() const {
    std::shared_lock lock(mutex_);

    std::vector<SimulatedOrder> result;
    result.reserve(active_orders_.size());

    for (const auto& [key, order] : active_orders_) {
        result.push_back(order);
    }

    return result;
}

std::optional<SimulatedOrder> PaperTradingMode::get_order(
    core::OrderId order_id) const {
    std::shared_lock lock(mutex_);

    auto it = orders_.find(order_id.value);
    if (it != orders_.end()) {
        return it->second;
    }

    return std::nullopt;
}

// ============================================================================
// Internal Methods
// ============================================================================

core::ErrorCode PaperTradingMode::validate_order(const OrderRequest& request) {
    if (request.exchange == core::Exchange::Unknown) {
        return core::ErrorCode::InvalidConfiguration;
    }

    if (request.symbol.empty()) {
        return core::ErrorCode::InvalidSymbol;
    }

    if (request.quantity.is_zero()) {
        return core::ErrorCode::InvalidQuantity;
    }

    if (request.type == core::OrderType::Limit && request.price.is_zero()) {
        return core::ErrorCode::InvalidPrice;
    }

    return core::ErrorCode::Success;
}

bool PaperTradingMode::check_balance(const OrderRequest& request) {
    std::shared_lock lock(mutex_);

    std::string quote_asset = get_quote_asset(request.symbol);
    std::string base_asset = get_base_asset(request.symbol);
    std::string key;

    if (request.side == core::Side::Buy) {
        // Buying: need quote currency
        key = std::string(core::exchange_to_string(request.exchange)) + ":" + quote_asset;
        auto it = balances_.find(key);
        if (it == balances_.end()) {
            return false;
        }

        // Calculate required balance
        core::Price price = request.type == core::OrderType::Market
            ? get_market_price_for_order(request)
            : request.price;

        // value = price * quantity / QUANTITY_SCALE
        uint64_t required = (price.value * request.quantity.value) / core::QUANTITY_SCALE;
        return it->second.available.value >= required;
    } else {
        // Selling: need base currency
        key = std::string(core::exchange_to_string(request.exchange)) + ":" + base_asset;
        auto it = balances_.find(key);
        if (it == balances_.end()) {
            // Check if we have a position to close
            SymbolKey sym_key{request.exchange, request.symbol};
            auto pos_it = positions_.find(sym_key);
            if (pos_it != positions_.end() && pos_it->second.is_long &&
                pos_it->second.quantity.value >= request.quantity.value) {
                return true;
            }
            return false;
        }

        return it->second.available.value >= request.quantity.value;
    }
}

void PaperTradingMode::reserve_balance(const OrderRequest& request) {
    std::unique_lock lock(mutex_);

    std::string quote_asset = get_quote_asset(request.symbol);
    std::string base_asset = get_base_asset(request.symbol);

    if (request.side == core::Side::Buy) {
        std::string key = std::string(core::exchange_to_string(request.exchange)) +
                         ":" + quote_asset;
        auto it = balances_.find(key);
        if (it != balances_.end()) {
            core::Price price = request.type == core::OrderType::Market
                ? core::Price{0}  // Market orders don't reserve until fill
                : request.price;

            if (price.value > 0) {
                uint64_t reserve = (price.value * request.quantity.value) / core::QUANTITY_SCALE;
                it->second.available.value -= std::min(reserve, it->second.available.value);
                it->second.locked.value += reserve;
            }
        }
    } else {
        std::string key = std::string(core::exchange_to_string(request.exchange)) +
                         ":" + base_asset;
        auto it = balances_.find(key);
        if (it != balances_.end()) {
            uint64_t reserve = request.quantity.value;
            it->second.available.value -= std::min(reserve, it->second.available.value);
            it->second.locked.value += reserve;
        }
    }
}

void PaperTradingMode::release_balance(const SimulatedOrder& order) {
    const auto& request = order.request;
    std::string quote_asset = get_quote_asset(request.symbol);
    std::string base_asset = get_base_asset(request.symbol);

    // Calculate unfilled portion
    uint64_t unfilled = request.quantity.value - order.filled_quantity.value;
    if (unfilled == 0) {
        return;
    }

    if (request.side == core::Side::Buy) {
        std::string key = std::string(core::exchange_to_string(request.exchange)) +
                         ":" + quote_asset;
        auto it = balances_.find(key);
        if (it != balances_.end() && request.price.value > 0) {
            uint64_t locked = (request.price.value * unfilled) / core::QUANTITY_SCALE;
            it->second.locked.value -= std::min(locked, it->second.locked.value);
            it->second.available.value += locked;
        }
    } else {
        std::string key = std::string(core::exchange_to_string(request.exchange)) +
                         ":" + base_asset;
        auto it = balances_.find(key);
        if (it != balances_.end()) {
            it->second.locked.value -= std::min(unfilled, it->second.locked.value);
            it->second.available.value += unfilled;
        }
    }
}

void PaperTradingMode::execute_market_order(SimulatedOrder& order) {
    SymbolKey key{order.request.exchange, order.request.symbol};
    auto price_it = market_prices_.find(key);

    core::Price fill_price;
    if (price_it != market_prices_.end() && price_it->second.valid) {
        fill_price = calculate_slippage(order.request, price_it->second);
    } else {
        // No market price available, reject order
        order.status = core::OrderStatus::Rejected;
        return;
    }

    // For market orders, fill entirely (or simulate partial fills)
    core::Quantity fill_quantity = order.request.quantity;

    // Optional partial fill simulation
    if (config_.partial_fill_probability > 0.0) {
        double roll = partial_fill_dist_(rng_);
        if (roll < config_.partial_fill_probability) {
            // Partial fill: 50-99% of quantity
            double fill_pct = 0.5 + (roll / config_.partial_fill_probability) * 0.49;
            fill_quantity = core::Quantity{
                static_cast<uint64_t>(order.request.quantity.value * fill_pct)};
        }
    }

    process_fill(order, fill_price, fill_quantity);
}

void PaperTradingMode::try_fill_limit_order(SimulatedOrder& order,
                                            const MarketPriceSnapshot& price) {
    if (!price.valid) {
        return;
    }

    bool should_fill = false;
    core::Price fill_price = order.request.price;

    if (order.request.side == core::Side::Buy) {
        // Buy limit: fill when ask <= limit price
        if (price.ask.value <= order.request.price.value) {
            should_fill = true;
            // Fill at limit price (maker) or ask (taker, if crossing)
            fill_price = order.request.price;
        }
    } else {
        // Sell limit: fill when bid >= limit price
        if (price.bid.value >= order.request.price.value) {
            should_fill = true;
            // Fill at limit price (maker)
            fill_price = order.request.price;
        }
    }

    if (should_fill) {
        // Calculate fill quantity (can simulate partial fills)
        core::Quantity remaining{order.request.quantity.value - order.filled_quantity.value};
        core::Quantity fill_quantity = remaining;

        // Optional partial fill simulation
        if (config_.partial_fill_probability > 0.0 && order.fill_count < 3) {
            double roll = partial_fill_dist_(rng_);
            if (roll < config_.partial_fill_probability) {
                double fill_pct = 0.3 + roll * 0.4;
                fill_quantity = core::Quantity{
                    static_cast<uint64_t>(remaining.value * fill_pct)};
                fill_quantity = core::Quantity{
                    std::max(fill_quantity.value, 1ULL)};
            }
        }

        process_fill(order, fill_price, fill_quantity);
    }
}

void PaperTradingMode::process_fill(SimulatedOrder& order, core::Price fill_price,
                                    core::Quantity fill_quantity) {
    // Update average fill price
    uint64_t prev_value = order.avg_fill_price.value * order.filled_quantity.value;
    uint64_t new_value = fill_price.value * fill_quantity.value;
    uint64_t total_quantity = order.filled_quantity.value + fill_quantity.value;

    if (total_quantity > 0) {
        order.avg_fill_price = core::Price{(prev_value + new_value) / total_quantity};
    }

    order.filled_quantity = core::Quantity{total_quantity};
    order.fill_count++;
    order.updated_at = current_timestamp();

    // Check if fully filled
    if (order.filled_quantity.value >= order.request.quantity.value) {
        order.status = core::OrderStatus::Filled;
        stats_orders_filled_++;
    } else {
        order.status = core::OrderStatus::PartiallyFilled;
    }

    // Calculate fee
    uint64_t fill_value = (fill_price.value * fill_quantity.value) / core::QUANTITY_SCALE;
    bool is_maker = (order.request.type != core::OrderType::Market);
    core::Price fee = calculate_fee(order.request.exchange,
                                   core::Price{fill_value}, is_maker);

    // Update statistics
    stats_total_volume_ += fill_value;
    stats_total_fees_ += fee.value;

    // Update position and balance
    update_position(order.request.exchange, order.request.symbol,
                   order.request.side, fill_price, fill_quantity);
    update_balance_after_fill(order.request.exchange, order.request.symbol,
                             order.request.side, fill_price, fill_quantity, fee);

    // Emit execution report
    ExecutionReport exec;
    exec.client_order_id = order.request.client_order_id;
    exec.exchange_order_id = order.exchange_order_id;
    exec.trade_id = core::TradeId{static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    exec.exchange = order.request.exchange;
    exec.symbol = order.request.symbol;
    exec.side = order.request.side;
    exec.status = order.status;
    exec.price = fill_price;
    exec.quantity = fill_quantity;
    exec.cumulative_quantity = order.filled_quantity;
    exec.remaining_quantity = core::Quantity{
        order.request.quantity.value - order.filled_quantity.value};
    exec.commission = fee;
    exec.commission_asset = get_quote_asset(order.request.symbol);
    exec.timestamp = current_timestamp();
    exec.is_maker = is_maker;

    emit_execution(exec);
}

core::Price PaperTradingMode::calculate_slippage(const OrderRequest& request,
                                                 const MarketPriceSnapshot& price) {
    core::Price base_price = request.side == core::Side::Buy ? price.ask : price.bid;

    if (config_.slippage_bps > 0.0) {
        double slippage_factor = config_.slippage_bps / 10000.0;
        if (request.side == core::Side::Buy) {
            // Buying: price goes up
            return core::Price{static_cast<uint64_t>(
                base_price.value * (1.0 + slippage_factor))};
        } else {
            // Selling: price goes down
            return core::Price{static_cast<uint64_t>(
                base_price.value * (1.0 - slippage_factor))};
        }
    }

    return base_price;
}

core::Price PaperTradingMode::calculate_fee(core::Exchange exchange,
                                            core::Price value, bool is_maker) {
    auto it = config_.fees.find(exchange);
    double fee_rate = 0.001;  // Default 0.1%

    if (it != config_.fees.end()) {
        fee_rate = is_maker ? it->second.maker_fee : it->second.taker_fee;
    }

    return core::Price{static_cast<uint64_t>(value.value * fee_rate)};
}

void PaperTradingMode::update_position(core::Exchange exchange,
                                        const core::Symbol& symbol,
                                        core::Side side, core::Price price,
                                        core::Quantity quantity) {
    SymbolKey key{exchange, symbol};
    Position& pos = positions_[key];
    pos.exchange = exchange;
    pos.symbol = symbol;

    if (pos.is_flat()) {
        // New position
        pos.quantity = quantity;
        pos.avg_entry_price = price;
        pos.is_long = (side == core::Side::Buy);
        pos.realized_pnl = core::Price{0};
    } else if ((side == core::Side::Buy && pos.is_long) ||
               (side == core::Side::Sell && !pos.is_long)) {
        // Adding to position
        uint64_t prev_value = pos.avg_entry_price.value * pos.quantity.value;
        uint64_t add_value = price.value * quantity.value;
        uint64_t new_quantity = pos.quantity.value + quantity.value;

        pos.avg_entry_price = core::Price{(prev_value + add_value) / new_quantity};
        pos.quantity = core::Quantity{new_quantity};
    } else {
        // Reducing position
        if (quantity.value >= pos.quantity.value) {
            // Closing or reversing
            // Calculate realized P&L
            int64_t pnl = 0;
            if (pos.is_long) {
                pnl = static_cast<int64_t>(price.value) -
                      static_cast<int64_t>(pos.avg_entry_price.value);
            } else {
                pnl = static_cast<int64_t>(pos.avg_entry_price.value) -
                      static_cast<int64_t>(price.value);
            }
            pnl = (pnl * static_cast<int64_t>(pos.quantity.value)) /
                  static_cast<int64_t>(core::QUANTITY_SCALE);
            stats_realized_pnl_ += pnl;
            pos.realized_pnl = core::Price{static_cast<uint64_t>(
                pos.realized_pnl.value + std::max(0LL, pnl))};

            uint64_t remaining = quantity.value - pos.quantity.value;
            if (remaining > 0) {
                // Reversing position
                pos.quantity = core::Quantity{remaining};
                pos.avg_entry_price = price;
                pos.is_long = (side == core::Side::Buy);
            } else {
                // Closed position
                pos.quantity = core::Quantity{0};
            }
        } else {
            // Partial close
            int64_t pnl = 0;
            if (pos.is_long) {
                pnl = static_cast<int64_t>(price.value) -
                      static_cast<int64_t>(pos.avg_entry_price.value);
            } else {
                pnl = static_cast<int64_t>(pos.avg_entry_price.value) -
                      static_cast<int64_t>(price.value);
            }
            pnl = (pnl * static_cast<int64_t>(quantity.value)) /
                  static_cast<int64_t>(core::QUANTITY_SCALE);
            stats_realized_pnl_ += pnl;
            pos.realized_pnl = core::Price{static_cast<uint64_t>(
                pos.realized_pnl.value + std::max(0LL, pnl))};

            pos.quantity = core::Quantity{pos.quantity.value - quantity.value};
        }
    }

    pos.updated_at = current_timestamp();
    emit_position_update(pos);
}

void PaperTradingMode::update_balance_after_fill(core::Exchange exchange,
                                                  const core::Symbol& symbol,
                                                  core::Side side,
                                                  core::Price price,
                                                  core::Quantity quantity,
                                                  core::Price fee) {
    std::string quote_asset = get_quote_asset(symbol);
    std::string base_asset = get_base_asset(symbol);

    uint64_t value = (price.value * quantity.value) / core::QUANTITY_SCALE;

    if (side == core::Side::Buy) {
        // Deduct quote currency, add base currency
        std::string quote_key = std::string(core::exchange_to_string(exchange)) +
                               ":" + quote_asset;
        std::string base_key = std::string(core::exchange_to_string(exchange)) +
                              ":" + base_asset;

        // Deduct quote
        auto quote_it = balances_.find(quote_key);
        if (quote_it != balances_.end()) {
            uint64_t total_deduct = value + fee.value;
            quote_it->second.total.value -= std::min(total_deduct,
                                                     quote_it->second.total.value);
            quote_it->second.available.value -= std::min(total_deduct,
                                                         quote_it->second.available.value);
            emit_balance_update(quote_it->second);
        }

        // Add base
        Balance& base_bal = balances_[base_key];
        if (base_bal.asset.empty()) {
            base_bal.asset = base_asset;
            base_bal.exchange = exchange;
        }
        base_bal.total.value += quantity.value;
        base_bal.available.value += quantity.value;
        base_bal.updated_at = current_timestamp();
        emit_balance_update(base_bal);
    } else {
        // Deduct base currency, add quote currency
        std::string quote_key = std::string(core::exchange_to_string(exchange)) +
                               ":" + quote_asset;
        std::string base_key = std::string(core::exchange_to_string(exchange)) +
                              ":" + base_asset;

        // Deduct base
        auto base_it = balances_.find(base_key);
        if (base_it != balances_.end()) {
            base_it->second.total.value -= std::min(quantity.value,
                                                    base_it->second.total.value);
            base_it->second.locked.value -= std::min(quantity.value,
                                                     base_it->second.locked.value);
            emit_balance_update(base_it->second);
        }

        // Add quote (minus fee)
        Balance& quote_bal = balances_[quote_key];
        if (quote_bal.asset.empty()) {
            quote_bal.asset = quote_asset;
            quote_bal.exchange = exchange;
        }
        quote_bal.total.value += (value - fee.value);
        quote_bal.available.value += (value - fee.value);
        quote_bal.updated_at = current_timestamp();
        emit_balance_update(quote_bal);
    }
}

std::string PaperTradingMode::get_quote_asset(const core::Symbol& symbol) {
    std::string_view sv = symbol.view();

    // Common quote assets to check
    static const std::array<std::string_view, 6> quotes = {
        "USDT", "USDC", "BUSD", "USD", "BTC", "ETH"
    };

    for (auto quote : quotes) {
        if (sv.length() > quote.length()) {
            if (sv.substr(sv.length() - quote.length()) == quote) {
                return std::string(quote);
            }
        }
    }

    // Default to USDT
    return "USDT";
}

std::string PaperTradingMode::get_base_asset(const core::Symbol& symbol) {
    std::string_view sv = symbol.view();
    std::string quote = get_quote_asset(symbol);

    if (sv.length() > quote.length()) {
        return std::string(sv.substr(0, sv.length() - quote.length()));
    }

    return std::string(sv);
}

core::OrderId PaperTradingMode::generate_exchange_order_id() {
    return core::OrderId{next_exchange_order_id_.fetch_add(1)};
}

void PaperTradingMode::fill_simulation_loop() {
    while (running_.load()) {
        std::shared_lock lock(mutex_);

        // Check all active orders for fills
        std::vector<uint64_t> filled_orders;

        for (auto& [key, order] : active_orders_) {
            if (order.status == core::OrderStatus::Filled ||
                order.status == core::OrderStatus::Cancelled) {
                filled_orders.push_back(key);
                continue;
            }

            SymbolKey sym_key{order.request.exchange, order.request.symbol};
            auto price_it = market_prices_.find(sym_key);
            if (price_it != market_prices_.end()) {
                lock.unlock();

                {
                    std::unique_lock write_lock(mutex_);
                    try_fill_limit_order(active_orders_[key], price_it->second);
                    if (active_orders_[key].status == core::OrderStatus::Filled) {
                        orders_[key] = active_orders_[key];
                        filled_orders.push_back(key);
                    }
                }

                lock.lock();
            }
        }

        // Remove filled orders from active orders
        lock.unlock();
        {
            std::unique_lock write_lock(mutex_);
            for (uint64_t key : filled_orders) {
                active_orders_.erase(key);
            }
        }

        // Wait for next market update or timeout
        std::unique_lock cv_lock(mutex_);
        fill_cv_.wait_for(cv_lock, std::chrono::milliseconds(10),
                         [this]() { return !running_.load(); });
    }
}

void PaperTradingMode::emit_order_response(const OrderResponse& response) {
    std::lock_guard lock(callback_mutex_);
    if (on_order_response_) {
        on_order_response_(response);
    }
}

void PaperTradingMode::emit_execution(const ExecutionReport& execution) {
    std::lock_guard lock(callback_mutex_);
    if (on_execution_) {
        on_execution_(execution);
    }
}

void PaperTradingMode::emit_cancel_response(const CancelResponse& response) {
    std::lock_guard lock(callback_mutex_);
    if (on_cancel_response_) {
        on_cancel_response_(response);
    }
}

void PaperTradingMode::emit_position_update(const Position& position) {
    std::lock_guard lock(callback_mutex_);
    if (on_position_update_) {
        on_position_update_(position);
    }
}

void PaperTradingMode::emit_balance_update(const Balance& balance) {
    std::lock_guard lock(callback_mutex_);
    if (on_balance_update_) {
        on_balance_update_(balance);
    }
}

void PaperTradingMode::emit_error(core::ErrorCode code, const std::string& message) {
    std::lock_guard lock(callback_mutex_);
    if (on_error_) {
        on_error_(code, message);
    }
}

// Helper function that was referenced but not defined
core::Price get_market_price_for_order(const OrderRequest& request) {
    // This is a fallback - in practice, market prices should be available
    return core::Price::from_double(0.0);
}

// ============================================================================
// Factory Function
// ============================================================================

std::shared_ptr<PaperTradingMode> create_paper_trading_mode() {
    return std::make_shared<PaperTradingMode>();
}

}  // namespace hft::trading
