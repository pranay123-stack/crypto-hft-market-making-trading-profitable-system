#include "trading/live_trading.hpp"

#include <algorithm>
#include <sstream>
#include <cmath>
#include <iostream>

namespace hft::trading {

// ============================================================================
// Rate Limiter Implementation
// ============================================================================

RateLimiter::RateLimiter(uint32_t tokens_per_second, uint32_t burst_capacity)
    : tokens_per_second_(tokens_per_second)
    , burst_capacity_(burst_capacity)
    , available_(burst_capacity)
    , last_refill_(std::chrono::steady_clock::now()) {
}

bool RateLimiter::try_acquire(uint32_t tokens) {
    std::lock_guard lock(mutex_);
    refill();

    if (available_.load() >= tokens) {
        available_ -= tokens;
        return true;
    }
    return false;
}

bool RateLimiter::acquire(uint32_t tokens, uint32_t timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        if (try_acquire(tokens)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
}

uint32_t RateLimiter::available_tokens() const {
    std::lock_guard lock(mutex_);
    const_cast<RateLimiter*>(this)->refill();
    return available_.load();
}

void RateLimiter::reset() {
    std::lock_guard lock(mutex_);
    available_ = burst_capacity_;
    last_refill_ = std::chrono::steady_clock::now();
}

void RateLimiter::refill() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        now - last_refill_).count();

    if (elapsed > 0) {
        // Calculate tokens to add based on elapsed time
        uint64_t tokens_to_add = (elapsed * tokens_per_second_) / 1'000'000;
        if (tokens_to_add > 0) {
            uint32_t new_tokens = std::min(
                available_.load() + static_cast<uint32_t>(tokens_to_add),
                burst_capacity_);
            available_ = new_tokens;
            last_refill_ = now;
        }
    }
}

// ============================================================================
// Live Trading Mode Implementation
// ============================================================================

LiveTradingMode::LiveTradingMode() = default;

LiveTradingMode::~LiveTradingMode() {
    stop();
}

// ============================================================================
// Lifecycle Management
// ============================================================================

bool LiveTradingMode::initialize(const TradingModeConfig& config) {
    std::unique_lock lock(mutex_);

    if (initialized_) {
        return true;
    }

    config_ = config;

    // Initialize default risk parameters if not set
    if (risk_params_.max_position_value.is_zero()) {
        risk_params_ = create_default_risk_parameters();
    }

    // Set default fees if not provided
    if (config_.fees.empty()) {
        FeeStructure default_fees;
        default_fees.maker_fee = 0.001;
        default_fees.taker_fee = 0.001;
        config_.fees[core::Exchange::Binance] = default_fees;
        config_.fees[core::Exchange::Coinbase] = default_fees;
        config_.fees[core::Exchange::Kraken] = default_fees;
    }

    initialized_ = true;
    return true;
}

bool LiveTradingMode::start() {
    if (!initialized_) {
        emit_error(core::ErrorCode::InvalidConfiguration,
                   "Live trading mode not initialized");
        return false;
    }

    if (running_.exchange(true)) {
        return true;  // Already running
    }

    // Initialize rate limiters for registered connectors
    for (const auto& [exchange, connector] : connectors_) {
        if (!rate_limiters_.count(exchange)) {
            rate_limiters_[exchange] = std::make_unique<RateLimiter>(
                risk_params_.max_orders_per_second,
                risk_params_.max_orders_per_second * 2);
        }
        if (!cancel_limiters_.count(exchange)) {
            cancel_limiters_[exchange] = std::make_unique<RateLimiter>(
                risk_params_.max_cancels_per_second,
                risk_params_.max_cancels_per_second * 2);
        }
    }

    // Start timeout check thread
    timeout_thread_ = std::make_unique<std::thread>([this]() {
        timeout_check_loop();
    });

    // Start sync thread
    sync_thread_ = std::make_unique<std::thread>([this]() {
        sync_loop();
    });

    // Don't automatically enable trading - require explicit enablement
    trading_enabled_ = config_.enable_trading;

    return true;
}

void LiveTradingMode::stop() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    // Disable trading
    trading_enabled_ = false;

    // Wake up threads
    shutdown_cv_.notify_all();

    // Wait for threads to finish
    if (timeout_thread_ && timeout_thread_->joinable()) {
        timeout_thread_->join();
    }
    timeout_thread_.reset();

    if (sync_thread_ && sync_thread_->joinable()) {
        sync_thread_->join();
    }
    sync_thread_.reset();

    // Disconnect connectors
    for (auto& [exchange, connector] : connectors_) {
        connector->disconnect();
    }
}

bool LiveTradingMode::is_running() const {
    return running_.load();
}

core::TradingMode LiveTradingMode::mode() const {
    return core::TradingMode::Live;
}

// ============================================================================
// Order Management
// ============================================================================

OrderResponse LiveTradingMode::submit_order(const OrderRequest& request) {
    OrderResponse response;
    response.client_order_id = request.client_order_id;
    response.exchange = request.exchange;
    response.symbol = request.symbol;
    response.timestamp = current_timestamp();
    response.user_data = request.user_data;

    // Check if trading is enabled
    if (!trading_enabled_.load()) {
        response.status = core::OrderStatus::Rejected;
        response.error_code = core::ErrorCode::OrderRejected;
        response.error_message = "Trading is disabled";
        stats_orders_rejected_++;
        emit_order_response(response);
        return response;
    }

    // Check if halted
    if (halted_.load()) {
        response.status = core::OrderStatus::Rejected;
        response.error_code = core::ErrorCode::OrderRejected;
        response.error_message = "Trading halted: " + halt_reason_;
        stats_orders_rejected_++;
        emit_order_response(response);
        return response;
    }

    // Perform safety check
    auto safety_result = perform_safety_check(request);
    if (!safety_result.passed) {
        response.status = core::OrderStatus::Rejected;
        response.error_code = safety_result.error_code;
        response.error_message = safety_result.error_message;
        stats_orders_rejected_++;
        emit_order_response(response);
        return response;
    }

    // Get connector
    auto connector = get_connector(request.exchange);
    if (!connector) {
        response.status = core::OrderStatus::Rejected;
        response.error_code = core::ErrorCode::ConnectionFailed;
        response.error_message = "No connector for exchange";
        stats_orders_rejected_++;
        emit_order_response(response);
        return response;
    }

    // Check rate limit
    if (!check_rate_limit(request.exchange)) {
        response.status = core::OrderStatus::Rejected;
        response.error_code = core::ErrorCode::RateLimited;
        response.error_message = "Rate limit exceeded";
        stats_orders_rejected_++;
        emit_order_response(response);
        return response;
    }

    // Dry run mode - log but don't execute
    if (config_.dry_run) {
        response.status = core::OrderStatus::New;
        response.exchange_order_id = generate_client_order_id();
        response.remaining_quantity = request.quantity;
        stats_orders_submitted_++;
        emit_order_response(response);
        return response;
    }

    // Execute order with retry logic
    response = execute_with_retry(connector, request);
    stats_orders_submitted_++;

    // Track pending order
    if (response.status == core::OrderStatus::Pending ||
        response.status == core::OrderStatus::New) {
        std::unique_lock lock(mutex_);
        PendingOrder pending;
        pending.request = request;
        pending.exchange_order_id = response.exchange_order_id;
        pending.sent_at = current_timestamp();
        pending.retry_count = 0;
        pending.confirmed = (response.status == core::OrderStatus::New);
        pending_orders_[response.exchange_order_id.value] = pending;
    }

    last_order_time_ = current_timestamp();
    emit_order_response(response);

    return response;
}

CancelResponse LiveTradingMode::cancel_order(const CancelRequest& request) {
    CancelResponse response;
    response.client_order_id = request.client_order_id;
    response.exchange_order_id = request.exchange_order_id;
    response.exchange = request.exchange;
    response.symbol = request.symbol;
    response.timestamp = current_timestamp();

    // Get connector
    auto connector = get_connector(request.exchange);
    if (!connector) {
        response.success = false;
        response.error_code = core::ErrorCode::ConnectionFailed;
        response.error_message = "No connector for exchange";
        emit_cancel_response(response);
        return response;
    }

    // Check cancel rate limit
    auto cancel_limiter = cancel_limiters_.find(request.exchange);
    if (cancel_limiter != cancel_limiters_.end()) {
        if (!cancel_limiter->second->try_acquire()) {
            response.success = false;
            response.error_code = core::ErrorCode::RateLimited;
            response.error_message = "Cancel rate limit exceeded";
            emit_cancel_response(response);
            return response;
        }
    }

    // Dry run mode
    if (config_.dry_run) {
        response.success = true;
        stats_orders_cancelled_++;
        emit_cancel_response(response);
        return response;
    }

    // Execute cancel
    response = connector->cancel_order(request);

    if (response.success) {
        stats_orders_cancelled_++;

        // Remove from pending
        std::unique_lock lock(mutex_);
        pending_orders_.erase(request.exchange_order_id.value);
        confirmed_orders_.erase(request.exchange_order_id.value);
    }

    emit_cancel_response(response);
    return response;
}

uint32_t LiveTradingMode::cancel_all_orders(core::Exchange exchange,
                                            const core::Symbol& symbol) {
    uint32_t cancelled = 0;

    std::vector<std::pair<core::Exchange, core::OrderId>> orders_to_cancel;

    {
        std::shared_lock lock(mutex_);
        for (const auto& [id, order] : confirmed_orders_) {
            bool match = true;

            if (exchange != core::Exchange::Unknown &&
                order.exchange != exchange) {
                match = false;
            }

            if (!symbol.empty() && !(order.symbol == symbol)) {
                match = false;
            }

            if (match) {
                orders_to_cancel.emplace_back(order.exchange,
                                             core::OrderId{id});
            }
        }
    }

    for (const auto& [ex, order_id] : orders_to_cancel) {
        CancelRequest request;
        request.exchange_order_id = order_id;
        request.exchange = ex;
        request.timestamp = current_timestamp();

        auto response = cancel_order(request);
        if (response.success) {
            cancelled++;
        }
    }

    return cancelled;
}

OrderResponse LiveTradingMode::modify_order(core::OrderId original_order_id,
                                            const OrderRequest& new_request) {
    // Cancel original order
    CancelRequest cancel_req;
    cancel_req.exchange_order_id = original_order_id;
    cancel_req.exchange = new_request.exchange;
    cancel_req.timestamp = current_timestamp();
    cancel_order(cancel_req);

    // Submit new order
    return submit_order(new_request);
}

// ============================================================================
// Position Management
// ============================================================================

std::optional<Position> LiveTradingMode::get_position(
    core::Exchange exchange, const core::Symbol& symbol) const {
    std::shared_lock lock(mutex_);

    std::string key = std::string(core::exchange_to_string(exchange)) + ":" +
                     std::string(symbol.view());
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<Position> LiveTradingMode::get_all_positions(
    core::Exchange exchange) const {
    std::shared_lock lock(mutex_);

    std::vector<Position> result;
    for (const auto& [key, pos] : positions_) {
        if (exchange == core::Exchange::Unknown || pos.exchange == exchange) {
            if (!pos.is_flat()) {
                result.push_back(pos);
            }
        }
    }

    return result;
}

OrderResponse LiveTradingMode::close_position(core::Exchange exchange,
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

uint32_t LiveTradingMode::close_all_positions(core::Exchange exchange) {
    std::vector<std::pair<core::Exchange, core::Symbol>> positions_to_close;

    {
        std::shared_lock lock(mutex_);
        for (const auto& [key, pos] : positions_) {
            if ((exchange == core::Exchange::Unknown || pos.exchange == exchange) &&
                !pos.is_flat()) {
                positions_to_close.emplace_back(pos.exchange, pos.symbol);
            }
        }
    }

    uint32_t closed = 0;
    for (const auto& [ex, sym] : positions_to_close) {
        auto response = close_position(ex, sym);
        if (response.status != core::OrderStatus::Rejected) {
            closed++;
        }
    }

    return closed;
}

// ============================================================================
// Balance Management
// ============================================================================

std::optional<Balance> LiveTradingMode::get_balance(
    core::Exchange exchange, const std::string& asset) const {
    std::shared_lock lock(mutex_);

    std::string key = std::string(core::exchange_to_string(exchange)) + ":" + asset;
    auto it = balances_.find(key);
    if (it != balances_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<Balance> LiveTradingMode::get_all_balances(
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

core::Price LiveTradingMode::get_total_equity(core::Exchange exchange) const {
    std::shared_lock lock(mutex_);

    uint64_t total = 0;

    for (const auto& [key, bal] : balances_) {
        if (exchange == core::Exchange::Unknown || bal.exchange == exchange) {
            // Simple: assume USDT is equity
            if (bal.asset == "USDT" || bal.asset == "USD" || bal.asset == "USDC") {
                total += bal.total.value;
            }
        }
    }

    return core::Price{total};
}

std::optional<AccountInfo> LiveTradingMode::get_account_info(
    core::Exchange exchange) const {
    AccountInfo info;
    info.exchange = exchange;
    info.balances = get_all_balances(exchange);
    info.positions = get_all_positions(exchange);
    info.total_equity = get_total_equity(exchange);
    info.can_trade = trading_enabled_.load() && !halted_.load();
    info.updated_at = current_timestamp();

    return info;
}

// ============================================================================
// Market Data Integration
// ============================================================================

void LiveTradingMode::update_market_price(core::Exchange exchange,
                                          const core::Symbol& symbol,
                                          core::Price bid,
                                          core::Price ask,
                                          core::Price last_price,
                                          core::Timestamp timestamp) {
    std::unique_lock lock(mutex_);

    std::string key = std::string(core::exchange_to_string(exchange)) + ":" +
                     std::string(symbol.view());

    MarketPrice& price = market_prices_[key];
    price.bid = bid;
    price.ask = ask;
    price.last = last_price;
    price.timestamp = timestamp;

    // Update unrealized P&L
    auto pos_key = key;
    auto pos_it = positions_.find(pos_key);
    if (pos_it != positions_.end() && !pos_it->second.is_flat()) {
        Position& pos = pos_it->second;
        core::Price current_price = pos.is_long ? bid : ask;

        int64_t pnl = 0;
        if (pos.is_long) {
            pnl = static_cast<int64_t>(current_price.value) -
                  static_cast<int64_t>(pos.avg_entry_price.value);
        } else {
            pnl = static_cast<int64_t>(pos.avg_entry_price.value) -
                  static_cast<int64_t>(current_price.value);
        }
        pnl = (pnl * static_cast<int64_t>(pos.quantity.value)) /
              static_cast<int64_t>(core::QUANTITY_SCALE);

        pos.unrealized_pnl = core::Price{static_cast<uint64_t>(std::max(0LL, pnl))};
        pos.updated_at = timestamp;
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

void LiveTradingMode::set_on_order_response(OnOrderResponseCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_order_response_ = std::move(callback);
}

void LiveTradingMode::set_on_execution(OnExecutionCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_execution_ = std::move(callback);
}

void LiveTradingMode::set_on_cancel_response(OnCancelResponseCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_cancel_response_ = std::move(callback);
}

void LiveTradingMode::set_on_position_update(OnPositionUpdateCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_position_update_ = std::move(callback);
}

void LiveTradingMode::set_on_balance_update(OnBalanceUpdateCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_balance_update_ = std::move(callback);
}

void LiveTradingMode::set_on_error(OnErrorCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_error_ = std::move(callback);
}

// ============================================================================
// Statistics and Diagnostics
// ============================================================================

const TradingModeConfig& LiveTradingMode::config() const {
    return config_;
}

uint64_t LiveTradingMode::orders_submitted() const {
    return stats_orders_submitted_.load();
}

uint64_t LiveTradingMode::orders_filled() const {
    return stats_orders_filled_.load();
}

uint64_t LiveTradingMode::orders_cancelled() const {
    return stats_orders_cancelled_.load();
}

uint64_t LiveTradingMode::orders_rejected() const {
    return stats_orders_rejected_.load();
}

core::Price LiveTradingMode::total_volume() const {
    return core::Price{stats_total_volume_.load()};
}

core::Price LiveTradingMode::total_fees() const {
    return core::Price{stats_total_fees_.load()};
}

core::Price LiveTradingMode::realized_pnl() const {
    int64_t pnl = stats_realized_pnl_.load();
    return core::Price{static_cast<uint64_t>(std::max(0LL, pnl))};
}

void LiveTradingMode::reset_statistics() {
    stats_orders_submitted_ = 0;
    stats_orders_filled_ = 0;
    stats_orders_cancelled_ = 0;
    stats_orders_rejected_ = 0;
    stats_total_volume_ = 0;
    stats_total_fees_ = 0;
    stats_realized_pnl_ = 0;
}

// ============================================================================
// Live Trading Specific
// ============================================================================

void LiveTradingMode::register_connector(ExchangeConnectorPtr connector) {
    if (!connector) {
        return;
    }

    std::unique_lock lock(mutex_);
    core::Exchange exchange = connector->exchange();
    connectors_[exchange] = connector;

    // Set up callbacks
    connector->set_on_execution([this](const ExecutionReport& exec) {
        handle_execution(exec);
    });

    connector->set_on_order_update([this](const OrderResponse& update) {
        handle_order_update(update);
    });
}

ExchangeConnectorPtr LiveTradingMode::get_connector(core::Exchange exchange) const {
    std::shared_lock lock(mutex_);
    auto it = connectors_.find(exchange);
    if (it != connectors_.end()) {
        return it->second;
    }
    return nullptr;
}

void LiveTradingMode::set_risk_parameters(const RiskParameters& params) {
    std::unique_lock lock(mutex_);
    risk_params_ = params;

    // Update rate limiters
    for (auto& [exchange, limiter] : rate_limiters_) {
        limiter = std::make_unique<RateLimiter>(
            params.max_orders_per_second,
            params.max_orders_per_second * 2);
    }

    for (auto& [exchange, limiter] : cancel_limiters_) {
        limiter = std::make_unique<RateLimiter>(
            params.max_cancels_per_second,
            params.max_cancels_per_second * 2);
    }
}

const RiskParameters& LiveTradingMode::risk_parameters() const {
    return risk_params_;
}

SafetyCheckResult LiveTradingMode::perform_safety_check(
    const OrderRequest& request) const {
    SafetyCheckResult result;

    // 1. Validate order
    auto error = const_cast<LiveTradingMode*>(this)->validate_order(request);
    if (error != core::ErrorCode::Success) {
        result.passed = false;
        result.error_code = error;
        result.error_message = std::string(core::error_code_to_string(error));
        return result;
    }

    // 2. Check exchange health
    if (risk_params_.check_exchange_status) {
        auto connector = get_connector(request.exchange);
        if (connector && !connector->is_exchange_healthy()) {
            result.passed = false;
            result.error_code = core::ErrorCode::ConnectionFailed;
            result.error_message = "Exchange not healthy";
            return result;
        }
    }

    // 3. Check market data freshness
    if (!const_cast<LiveTradingMode*>(this)->check_market_data_freshness(
            request.exchange, request.symbol)) {
        result.passed = false;
        result.error_code = core::ErrorCode::StaleMarketData;
        result.error_message = "Market data too old";
        return result;
    }

    // 4. Check balance
    if (!const_cast<LiveTradingMode*>(this)->check_balance(request)) {
        result.passed = false;
        result.error_code = core::ErrorCode::InsufficientBalance;
        result.error_message = "Insufficient balance";
        return result;
    }

    // 5. Check position limits
    if (!const_cast<LiveTradingMode*>(this)->check_position_limits(request)) {
        result.passed = false;
        result.error_code = core::ErrorCode::OrderRejected;
        result.error_message = "Position limit exceeded";
        return result;
    }

    // 6. Check order value
    if (!request.price.is_zero()) {
        uint64_t order_value = (request.price.value * request.quantity.value) /
                               core::QUANTITY_SCALE;
        if (order_value > risk_params_.max_order_value.value) {
            result.passed = false;
            result.error_code = core::ErrorCode::OrderRejected;
            result.error_message = "Order value exceeds limit";
            return result;
        }
    }

    // 7. Check open orders count
    {
        std::shared_lock lock(mutex_);
        if (confirmed_orders_.size() >= risk_params_.max_open_orders) {
            result.passed = false;
            result.error_code = core::ErrorCode::OrderRejected;
            result.error_message = "Too many open orders";
            return result;
        }
    }

    // 8. Check spread
    if (risk_params_.require_two_sided_market) {
        std::shared_lock lock(mutex_);
        std::string key = std::string(core::exchange_to_string(request.exchange)) +
                         ":" + std::string(request.symbol.view());
        auto it = market_prices_.find(key);
        if (it != market_prices_.end()) {
            if (it->second.bid.is_zero() || it->second.ask.is_zero()) {
                result.passed = false;
                result.error_code = core::ErrorCode::InvalidMarketData;
                result.error_message = "No two-sided market";
                return result;
            }

            double spread_pct = 100.0 * (it->second.ask.to_double() -
                                        it->second.bid.to_double()) /
                               it->second.bid.to_double();
            if (spread_pct > risk_params_.max_spread_percent) {
                result.warnings.push_back("Wide spread: " +
                                         std::to_string(spread_pct) + "%");
            }
        }
    }

    // 9. Check minimum order interval
    auto now = current_timestamp();
    auto last = last_order_time_.load();
    if (now.nanos - last.nanos < risk_params_.min_order_interval_us * 1000) {
        result.warnings.push_back("Rapid order submission");
    }

    return result;
}

bool LiveTradingMode::synchronize_all() {
    bool success = true;

    for (auto& [exchange, connector] : connectors_) {
        if (connector->is_connected()) {
            if (!connector->synchronize()) {
                success = false;
                emit_error(core::ErrorCode::InternalError,
                          "Failed to sync with " +
                          std::string(core::exchange_to_string(exchange)));
            }

            // Update local balances
            auto balances = connector->get_balances();
            {
                std::unique_lock lock(mutex_);
                for (const auto& bal : balances) {
                    std::string key = std::string(
                        core::exchange_to_string(bal.exchange)) + ":" + bal.asset;
                    balances_[key] = bal;
                }
            }

            // Update local positions
            auto positions = connector->get_positions();
            {
                std::unique_lock lock(mutex_);
                for (const auto& pos : positions) {
                    std::string key = std::string(
                        core::exchange_to_string(pos.exchange)) + ":" +
                        std::string(pos.symbol.view());
                    positions_[key] = pos;
                }
            }
        }
    }

    return success;
}

void LiveTradingMode::set_trading_enabled(bool enabled) {
    trading_enabled_ = enabled;
}

bool LiveTradingMode::is_trading_enabled() const {
    return trading_enabled_.load();
}

void LiveTradingMode::emergency_stop() {
    // Disable trading immediately
    trading_enabled_ = false;
    halted_ = true;
    halt_reason_ = "Emergency stop triggered";

    // Cancel all orders
    for (auto& [exchange, connector] : connectors_) {
        cancel_all_orders(exchange);
    }

    // Close all positions
    close_all_positions();

    emit_error(core::ErrorCode::InternalError, "Emergency stop executed");
}

std::vector<PendingOrder> LiveTradingMode::get_pending_orders() const {
    std::shared_lock lock(mutex_);

    std::vector<PendingOrder> result;
    result.reserve(pending_orders_.size());

    for (const auto& [id, order] : pending_orders_) {
        result.push_back(order);
    }

    return result;
}

core::Price LiveTradingMode::daily_pnl() const {
    int64_t pnl = stats_daily_pnl_.load();
    return core::Price{static_cast<uint64_t>(std::max(0LL, pnl))};
}

void LiveTradingMode::reset_daily_pnl() {
    stats_daily_pnl_ = 0;
    stats_peak_equity_ = get_total_equity().value;
}

bool LiveTradingMode::is_halted() const {
    return halted_.load();
}

std::string LiveTradingMode::halt_reason() const {
    return halt_reason_;
}

void LiveTradingMode::clear_halt() {
    halted_ = false;
    halt_reason_.clear();
}

// ============================================================================
// Internal Methods
// ============================================================================

core::ErrorCode LiveTradingMode::validate_order(const OrderRequest& request) {
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

bool LiveTradingMode::check_rate_limit(core::Exchange exchange) {
    auto it = rate_limiters_.find(exchange);
    if (it != rate_limiters_.end()) {
        return it->second->try_acquire();
    }
    return true;  // No limiter = no limit
}

bool LiveTradingMode::check_balance(const OrderRequest& request) {
    std::shared_lock lock(mutex_);

    std::string quote_asset = get_quote_asset(request.symbol);
    std::string base_asset = get_base_asset(request.symbol);

    if (request.side == core::Side::Buy) {
        std::string key = std::string(core::exchange_to_string(request.exchange)) +
                         ":" + quote_asset;
        auto it = balances_.find(key);
        if (it == balances_.end()) {
            return false;
        }

        core::Price price = request.price;
        if (price.is_zero()) {
            // Market order - estimate from market price
            std::string price_key = std::string(
                core::exchange_to_string(request.exchange)) + ":" +
                std::string(request.symbol.view());
            auto price_it = market_prices_.find(price_key);
            if (price_it != market_prices_.end()) {
                price = price_it->second.ask;
            } else {
                return false;  // Can't estimate price
            }
        }

        uint64_t required = (price.value * request.quantity.value) /
                           core::QUANTITY_SCALE;
        return it->second.available.value >= required;
    } else {
        std::string key = std::string(core::exchange_to_string(request.exchange)) +
                         ":" + base_asset;
        auto it = balances_.find(key);
        if (it == balances_.end()) {
            // Check position for reduce-only
            std::string pos_key = std::string(
                core::exchange_to_string(request.exchange)) + ":" +
                std::string(request.symbol.view());
            auto pos_it = positions_.find(pos_key);
            if (pos_it != positions_.end() && pos_it->second.is_long &&
                pos_it->second.quantity.value >= request.quantity.value) {
                return true;
            }
            return false;
        }

        return it->second.available.value >= request.quantity.value;
    }
}

bool LiveTradingMode::check_position_limits(const OrderRequest& request) {
    std::shared_lock lock(mutex_);

    std::string pos_key = std::string(core::exchange_to_string(request.exchange)) +
                         ":" + std::string(request.symbol.view());
    auto pos_it = positions_.find(pos_key);

    core::Quantity current_qty{0};
    if (pos_it != positions_.end()) {
        current_qty = pos_it->second.quantity;
    }

    // Check if order would exceed position value limit
    core::Quantity new_qty = current_qty;
    if ((request.side == core::Side::Buy && (pos_it == positions_.end() ||
         pos_it->second.is_long)) ||
        (request.side == core::Side::Sell && pos_it != positions_.end() &&
         !pos_it->second.is_long)) {
        new_qty = core::Quantity{new_qty.value + request.quantity.value};
    }

    // Estimate position value
    std::string price_key = pos_key;
    auto price_it = market_prices_.find(price_key);
    if (price_it != market_prices_.end()) {
        uint64_t pos_value = (price_it->second.last.value * new_qty.value) /
                            core::QUANTITY_SCALE;
        if (pos_value > risk_params_.max_position_value.value) {
            return false;
        }
    }

    return true;
}

bool LiveTradingMode::check_market_data_freshness(core::Exchange exchange,
                                                   const core::Symbol& symbol) {
    std::shared_lock lock(mutex_);

    std::string key = std::string(core::exchange_to_string(exchange)) + ":" +
                     std::string(symbol.view());
    auto it = market_prices_.find(key);

    if (it == market_prices_.end()) {
        return false;  // No market data
    }

    auto now = current_timestamp();
    uint64_t age_ms = (now.nanos - it->second.timestamp.nanos) / 1'000'000;

    return age_ms <= risk_params_.stale_price_threshold_ms;
}

bool LiveTradingMode::check_exchange_health(core::Exchange exchange) {
    auto connector = get_connector(exchange);
    if (!connector) {
        return false;
    }
    return connector->is_connected() && connector->is_exchange_healthy();
}

OrderResponse LiveTradingMode::execute_with_retry(ExchangeConnectorPtr connector,
                                                  const OrderRequest& request) {
    OrderResponse response;
    uint32_t retries = 0;

    while (retries <= config_.max_retries) {
        response = connector->submit_order(request);

        if (response.status != core::OrderStatus::Error &&
            response.error_code == core::ErrorCode::Success) {
            return response;
        }

        // Check if error is retryable
        bool retryable = (response.error_code == core::ErrorCode::ConnectionTimeout ||
                         response.error_code == core::ErrorCode::RateLimited ||
                         response.error_code == core::ErrorCode::Timeout);

        if (!retryable) {
            break;
        }

        retries++;
        if (retries <= config_.max_retries) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.retry_delay_ms * retries));
        }
    }

    return response;
}

void LiveTradingMode::handle_execution(const ExecutionReport& execution) {
    // Update position
    update_position_from_execution(execution);

    // Update balance
    update_balance_from_execution(execution);

    // Update statistics
    stats_total_volume_ += (execution.price.value * execution.quantity.value) /
                           core::QUANTITY_SCALE;
    stats_total_fees_ += execution.commission.value;

    if (execution.status == core::OrderStatus::Filled) {
        stats_orders_filled_++;

        // Remove from pending
        std::unique_lock lock(mutex_);
        pending_orders_.erase(execution.exchange_order_id.value);
        confirmed_orders_.erase(execution.exchange_order_id.value);
    }

    // Check risk limits
    check_risk_limits();

    // Emit callback
    emit_execution(execution);
}

void LiveTradingMode::handle_order_update(const OrderResponse& update) {
    {
        std::unique_lock lock(mutex_);

        // Update pending order status
        auto pending_it = pending_orders_.find(update.exchange_order_id.value);
        if (pending_it != pending_orders_.end()) {
            pending_it->second.confirmed = true;

            if (core::is_terminal_status(update.status)) {
                pending_orders_.erase(pending_it);
            }
        }

        // Track confirmed orders
        if (update.status == core::OrderStatus::New ||
            update.status == core::OrderStatus::PartiallyFilled) {
            confirmed_orders_[update.exchange_order_id.value] = update;
        } else if (core::is_terminal_status(update.status)) {
            confirmed_orders_.erase(update.exchange_order_id.value);
        }
    }

    // Emit callback
    emit_order_response(update);
}

void LiveTradingMode::update_position_from_execution(
    const ExecutionReport& execution) {
    std::unique_lock lock(mutex_);

    std::string key = std::string(core::exchange_to_string(execution.exchange)) +
                     ":" + std::string(execution.symbol.view());

    Position& pos = positions_[key];
    pos.exchange = execution.exchange;
    pos.symbol = execution.symbol;

    if (pos.is_flat()) {
        pos.quantity = execution.quantity;
        pos.avg_entry_price = execution.price;
        pos.is_long = (execution.side == core::Side::Buy);
    } else if ((execution.side == core::Side::Buy && pos.is_long) ||
               (execution.side == core::Side::Sell && !pos.is_long)) {
        uint64_t prev_value = pos.avg_entry_price.value * pos.quantity.value;
        uint64_t add_value = execution.price.value * execution.quantity.value;
        uint64_t new_quantity = pos.quantity.value + execution.quantity.value;

        pos.avg_entry_price = core::Price{(prev_value + add_value) / new_quantity};
        pos.quantity = core::Quantity{new_quantity};
    } else {
        if (execution.quantity.value >= pos.quantity.value) {
            // Calculate realized P&L
            int64_t pnl = 0;
            if (pos.is_long) {
                pnl = static_cast<int64_t>(execution.price.value) -
                      static_cast<int64_t>(pos.avg_entry_price.value);
            } else {
                pnl = static_cast<int64_t>(pos.avg_entry_price.value) -
                      static_cast<int64_t>(execution.price.value);
            }
            pnl = (pnl * static_cast<int64_t>(pos.quantity.value)) /
                  static_cast<int64_t>(core::QUANTITY_SCALE);
            stats_realized_pnl_ += pnl;
            stats_daily_pnl_ += pnl;

            uint64_t remaining = execution.quantity.value - pos.quantity.value;
            if (remaining > 0) {
                pos.quantity = core::Quantity{remaining};
                pos.avg_entry_price = execution.price;
                pos.is_long = (execution.side == core::Side::Buy);
            } else {
                pos.quantity = core::Quantity{0};
            }
        } else {
            int64_t pnl = 0;
            if (pos.is_long) {
                pnl = static_cast<int64_t>(execution.price.value) -
                      static_cast<int64_t>(pos.avg_entry_price.value);
            } else {
                pnl = static_cast<int64_t>(pos.avg_entry_price.value) -
                      static_cast<int64_t>(execution.price.value);
            }
            pnl = (pnl * static_cast<int64_t>(execution.quantity.value)) /
                  static_cast<int64_t>(core::QUANTITY_SCALE);
            stats_realized_pnl_ += pnl;
            stats_daily_pnl_ += pnl;

            pos.quantity = core::Quantity{pos.quantity.value - execution.quantity.value};
        }
    }

    pos.updated_at = execution.timestamp;

    lock.unlock();
    emit_position_update(pos);
}

void LiveTradingMode::update_balance_from_execution(
    const ExecutionReport& execution) {
    // Balances are synced from exchange - just request a sync
    auto connector = get_connector(execution.exchange);
    if (connector) {
        auto balances = connector->get_balances();
        {
            std::unique_lock lock(mutex_);
            for (const auto& bal : balances) {
                std::string key = std::string(
                    core::exchange_to_string(bal.exchange)) + ":" + bal.asset;
                balances_[key] = bal;
                emit_balance_update(bal);
            }
        }
    }
}

void LiveTradingMode::check_risk_limits() {
    // Check daily loss
    if (risk_params_.max_daily_loss.value > 0) {
        int64_t daily_pnl = stats_daily_pnl_.load();
        if (daily_pnl < -static_cast<int64_t>(risk_params_.max_daily_loss.value)) {
            halted_ = true;
            halt_reason_ = "Daily loss limit exceeded";
            trading_enabled_ = false;
            emit_error(core::ErrorCode::OrderRejected, halt_reason_);
            return;
        }
    }

    // Check drawdown
    if (risk_params_.max_drawdown.value > 0) {
        uint64_t current_equity = get_total_equity().value;
        uint64_t peak = stats_peak_equity_.load();

        if (current_equity > peak) {
            stats_peak_equity_ = current_equity;
        } else if (peak > 0) {
            uint64_t drawdown = peak - current_equity;
            if (drawdown > risk_params_.max_drawdown.value) {
                halted_ = true;
                halt_reason_ = "Drawdown limit exceeded";
                trading_enabled_ = false;
                emit_error(core::ErrorCode::OrderRejected, halt_reason_);
                return;
            }
        }
    }
}

void LiveTradingMode::timeout_check_loop() {
    constexpr uint32_t ORDER_TIMEOUT_MS = 30000;  // 30 seconds

    while (running_.load()) {
        {
            std::unique_lock lock(mutex_);

            auto now = current_timestamp();
            std::vector<uint64_t> timed_out;

            for (auto& [id, pending] : pending_orders_) {
                if (!pending.confirmed) {
                    uint64_t age_ms = (now.nanos - pending.sent_at.nanos) / 1'000'000;
                    if (age_ms > ORDER_TIMEOUT_MS) {
                        pending.timed_out = true;
                        timed_out.push_back(id);
                    }
                }
            }

            // Handle timed out orders
            for (uint64_t id : timed_out) {
                auto& pending = pending_orders_[id];
                emit_error(core::ErrorCode::Timeout,
                          "Order timed out: " + std::to_string(id));

                // Try to cancel
                CancelRequest cancel_req;
                cancel_req.exchange_order_id = core::OrderId{id};
                cancel_req.exchange = pending.request.exchange;
                cancel_req.timestamp = now;

                lock.unlock();
                cancel_order(cancel_req);
                lock.lock();
            }
        }

        // Sleep for a while
        std::unique_lock cv_lock(mutex_);
        shutdown_cv_.wait_for(cv_lock, std::chrono::seconds(1),
                             [this]() { return !running_.load(); });
    }
}

void LiveTradingMode::sync_loop() {
    constexpr uint32_t SYNC_INTERVAL_MS = 60000;  // 1 minute

    while (running_.load()) {
        synchronize_all();

        std::unique_lock cv_lock(mutex_);
        shutdown_cv_.wait_for(cv_lock, std::chrono::milliseconds(SYNC_INTERVAL_MS),
                             [this]() { return !running_.load(); });
    }
}

void LiveTradingMode::emit_order_response(const OrderResponse& response) {
    std::lock_guard lock(callback_mutex_);
    if (on_order_response_) {
        on_order_response_(response);
    }
}

void LiveTradingMode::emit_execution(const ExecutionReport& execution) {
    std::lock_guard lock(callback_mutex_);
    if (on_execution_) {
        on_execution_(execution);
    }
}

void LiveTradingMode::emit_cancel_response(const CancelResponse& response) {
    std::lock_guard lock(callback_mutex_);
    if (on_cancel_response_) {
        on_cancel_response_(response);
    }
}

void LiveTradingMode::emit_position_update(const Position& position) {
    std::lock_guard lock(callback_mutex_);
    if (on_position_update_) {
        on_position_update_(position);
    }
}

void LiveTradingMode::emit_balance_update(const Balance& balance) {
    std::lock_guard lock(callback_mutex_);
    if (on_balance_update_) {
        on_balance_update_(balance);
    }
}

void LiveTradingMode::emit_error(core::ErrorCode code, const std::string& message) {
    std::lock_guard lock(callback_mutex_);
    if (on_error_) {
        on_error_(code, message);
    }
}

std::string LiveTradingMode::get_quote_asset(const core::Symbol& symbol) {
    std::string_view sv = symbol.view();

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

    return "USDT";
}

std::string LiveTradingMode::get_base_asset(const core::Symbol& symbol) {
    std::string_view sv = symbol.view();
    std::string quote = get_quote_asset(symbol);

    if (sv.length() > quote.length()) {
        return std::string(sv.substr(0, sv.length() - quote.length()));
    }

    return std::string(sv);
}

// ============================================================================
// Factory Functions
// ============================================================================

std::shared_ptr<LiveTradingMode> create_live_trading_mode() {
    return std::make_shared<LiveTradingMode>();
}

RiskParameters create_default_risk_parameters() {
    RiskParameters params;

    // Position limits
    params.max_position_value = core::Price::from_double(100000.0);  // $100k
    params.max_total_exposure = core::Price::from_double(500000.0);  // $500k
    params.max_order_quantity = core::Quantity::from_double(100.0);
    params.max_order_value = core::Price::from_double(50000.0);      // $50k

    // Order limits
    params.max_open_orders = 100;
    params.max_orders_per_symbol = 10;

    // Loss limits
    params.max_daily_loss = core::Price::from_double(10000.0);       // $10k
    params.max_drawdown = core::Price::from_double(25000.0);         // $25k
    params.max_loss_percent = 5.0;

    // Rate limits
    params.max_orders_per_second = 10;
    params.max_orders_per_minute = 300;
    params.max_cancels_per_second = 20;

    // Timing
    params.min_order_interval_us = 10000;       // 10ms
    params.stale_price_threshold_ms = 5000;     // 5s

    // Operational
    params.require_two_sided_market = true;
    params.max_spread_percent = 5.0;
    params.check_exchange_status = true;

    return params;
}

}  // namespace hft::trading
