#include "risk/risk_manager.hpp"
#include <chrono>
#include <cmath>

namespace hft {

// ============================================================================
// RiskManager Implementation
// ============================================================================

RiskManager::RiskManager(const RiskLimits& limits)
    : limits_(limits)
{
}

RiskCheckResult RiskManager::check_order(const Order& order) {
    return check_order(order, 0);
}

RiskCheckResult RiskManager::check_order(const Order& order, Price reference_price) {
    orders_checked_++;

    // Check kill switch
    if (kill_switch_active_) {
        orders_rejected_++;
        return RiskCheckResult::fail(RiskViolation::KILL_SWITCH_ACTIVE,
                                     "Kill switch is active");
    }

    // Check if symbol is enabled
    if (!is_symbol_enabled(order.symbol)) {
        orders_rejected_++;
        return RiskCheckResult::fail(RiskViolation::SYMBOL_DISABLED,
                                     "Symbol trading is disabled");
    }

    // Position limit check
    auto pos_check = check_position_limit(order);
    if (!pos_check.passed) {
        orders_rejected_++;
        return pos_check;
    }

    // Order size check
    auto size_check = check_order_size(order);
    if (!size_check.passed) {
        orders_rejected_++;
        return size_check;
    }

    // Rate limit check
    auto rate_check = check_rate_limit();
    if (!rate_check.passed) {
        orders_rejected_++;
        return rate_check;
    }

    // Open orders check
    auto open_check = check_open_orders();
    if (!open_check.passed) {
        orders_rejected_++;
        return open_check;
    }

    // Daily loss check
    auto loss_check = check_daily_loss();
    if (!loss_check.passed) {
        orders_rejected_++;
        return loss_check;
    }

    // Price deviation check (if reference provided)
    if (reference_price > 0) {
        auto price_check = check_price_deviation(order, reference_price);
        if (!price_check.passed) {
            orders_rejected_++;
            return price_check;
        }
    }

    return RiskCheckResult::pass();
}

RiskCheckResult RiskManager::check_position_limit(const Order& order) {
    if (limits_.max_position_qty == 0) {
        return RiskCheckResult::pass();
    }

    std::lock_guard<std::mutex> lock(position_mutex_);

    auto it = positions_.find(order.symbol.str());
    Quantity current_pos = (it != positions_.end()) ? it->second.quantity : 0;

    Quantity potential_pos = current_pos;
    if (order.side == Side::BUY) {
        potential_pos += order.quantity;
    } else {
        potential_pos -= order.quantity;
    }

    if (std::abs(potential_pos) > limits_.max_position_qty) {
        return RiskCheckResult::fail(RiskViolation::POSITION_LIMIT,
            "Position limit exceeded: potential=" + std::to_string(potential_pos) +
            " max=" + std::to_string(limits_.max_position_qty));
    }

    return RiskCheckResult::pass();
}

RiskCheckResult RiskManager::check_order_size(const Order& order) {
    if (limits_.max_order_qty > 0 && order.quantity > limits_.max_order_qty) {
        return RiskCheckResult::fail(RiskViolation::ORDER_SIZE_LIMIT,
            "Order size exceeds limit: qty=" + std::to_string(order.quantity) +
            " max=" + std::to_string(limits_.max_order_qty));
    }

    if (limits_.max_order_value > 0) {
        double value = from_qty(order.quantity) * from_price(order.price);
        if (value > limits_.max_order_value) {
            return RiskCheckResult::fail(RiskViolation::ORDER_VALUE_LIMIT,
                "Order value exceeds limit: value=" + std::to_string(value) +
                " max=" + std::to_string(limits_.max_order_value));
        }
    }

    return RiskCheckResult::pass();
}

RiskCheckResult RiskManager::check_rate_limit() {
    if (limits_.max_orders_per_second == 0) {
        return RiskCheckResult::pass();
    }

    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    uint64_t prev_second = current_second_.load(std::memory_order_relaxed);

    if (now != prev_second) {
        // New second, reset counter
        if (current_second_.compare_exchange_strong(prev_second, now)) {
            orders_this_second_.store(0, std::memory_order_relaxed);
        }
    }

    uint32_t count = orders_this_second_.fetch_add(1, std::memory_order_relaxed);

    if (count >= limits_.max_orders_per_second) {
        return RiskCheckResult::fail(RiskViolation::RATE_LIMIT,
            "Rate limit exceeded: " + std::to_string(count) +
            " orders/second");
    }

    return RiskCheckResult::pass();
}

RiskCheckResult RiskManager::check_open_orders() {
    if (limits_.max_open_orders == 0) {
        return RiskCheckResult::pass();
    }

    uint32_t current = open_orders_.load(std::memory_order_relaxed);

    if (current >= limits_.max_open_orders) {
        return RiskCheckResult::fail(RiskViolation::OPEN_ORDERS_LIMIT,
            "Open orders limit reached: " + std::to_string(current));
    }

    return RiskCheckResult::pass();
}

RiskCheckResult RiskManager::check_daily_loss() {
    if (limits_.max_daily_loss == 0) {
        return RiskCheckResult::pass();
    }

    double daily_loss = -daily_realized_pnl_.load(std::memory_order_relaxed);

    if (daily_loss >= limits_.max_daily_loss) {
        // Activate kill switch
        activate_kill_switch("Daily loss limit reached: " + std::to_string(daily_loss));
        return RiskCheckResult::fail(RiskViolation::DAILY_LOSS_LIMIT,
            "Daily loss limit reached");
    }

    return RiskCheckResult::pass();
}

RiskCheckResult RiskManager::check_price_deviation(const Order& order, Price reference) {
    if (limits_.max_deviation_bps == 0 || reference == 0) {
        return RiskCheckResult::pass();
    }

    double deviation_bps = 10000.0 * std::abs(
        static_cast<double>(order.price - reference) / static_cast<double>(reference));

    if (deviation_bps > limits_.max_deviation_bps) {
        return RiskCheckResult::fail(RiskViolation::PRICE_DEVIATION,
            "Price deviation too high: " + std::to_string(deviation_bps) + " bps");
    }

    return RiskCheckResult::pass();
}

void RiskManager::on_order_sent(const Order& order) {
    open_orders_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(position_mutex_);
    open_orders_map_[order.id] = order;
}

void RiskManager::on_order_filled(const Order& order, Quantity filled_qty, Price fill_price) {
    std::lock_guard<std::mutex> lock(position_mutex_);

    std::string sym = order.symbol.str();
    Position& pos = positions_[sym];
    pos.symbol = order.symbol;

    Quantity old_qty = pos.quantity;
    Price old_avg = pos.avg_price;

    if (order.side == Side::BUY) {
        // Buying
        if (pos.quantity >= 0) {
            // Adding to long or opening long
            Quantity new_qty = pos.quantity + filled_qty;
            if (new_qty > 0) {
                pos.avg_price = (old_avg * old_qty + fill_price * filled_qty) / new_qty;
            }
            pos.quantity = new_qty;
        } else {
            // Covering short
            Quantity covered = std::min(filled_qty, -pos.quantity);
            double pnl = from_qty(covered) * (from_price(pos.avg_price) - from_price(fill_price));
            pos.realized_pnl += pnl;
            daily_realized_pnl_.fetch_add(pnl, std::memory_order_relaxed);

            pos.quantity += filled_qty;
            if (pos.quantity > 0) {
                // Flipped to long
                pos.avg_price = fill_price;
            }
        }
    } else {
        // Selling
        if (pos.quantity <= 0) {
            // Adding to short or opening short
            Quantity new_qty = pos.quantity - filled_qty;
            if (new_qty < 0) {
                pos.avg_price = (old_avg * (-old_qty) + fill_price * filled_qty) / (-new_qty);
            }
            pos.quantity = new_qty;
        } else {
            // Closing long
            Quantity closed = std::min(filled_qty, pos.quantity);
            double pnl = from_qty(closed) * (from_price(fill_price) - from_price(pos.avg_price));
            pos.realized_pnl += pnl;
            daily_realized_pnl_.fetch_add(pnl, std::memory_order_relaxed);

            pos.quantity -= filled_qty;
            if (pos.quantity < 0) {
                // Flipped to short
                pos.avg_price = fill_price;
            }
        }
    }

    pos.last_update = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Update drawdown tracking
    double equity = get_total_pnl();
    double peak = peak_equity_.load(std::memory_order_relaxed);
    if (equity > peak) {
        peak_equity_.store(equity, std::memory_order_relaxed);
    } else if (limits_.max_drawdown > 0) {
        double drawdown = peak - equity;
        if (drawdown > limits_.max_drawdown) {
            activate_kill_switch("Drawdown limit exceeded: " + std::to_string(drawdown));
        }
    }

    // Check if order is fully filled
    auto it = open_orders_map_.find(order.id);
    if (it != open_orders_map_.end()) {
        it->second.filled_qty += filled_qty;
        if (it->second.filled_qty >= it->second.quantity) {
            open_orders_map_.erase(it);
            open_orders_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

void RiskManager::on_order_canceled(OrderId order_id) {
    std::lock_guard<std::mutex> lock(position_mutex_);

    auto it = open_orders_map_.find(order_id);
    if (it != open_orders_map_.end()) {
        open_orders_map_.erase(it);
        open_orders_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void RiskManager::on_order_rejected(OrderId order_id) {
    on_order_canceled(order_id);

    uint32_t rejects = reject_count_.fetch_add(1, std::memory_order_relaxed);

    if (limits_.kill_switch_enabled && rejects >= limits_.reject_threshold) {
        activate_kill_switch("Too many order rejections: " + std::to_string(rejects));
    }
}

void RiskManager::update_position(const Symbol& symbol, Quantity qty, Price price) {
    std::lock_guard<std::mutex> lock(position_mutex_);

    Position& pos = positions_[symbol.str()];
    pos.symbol = symbol;
    pos.quantity = qty;
    pos.avg_price = price;
    pos.last_update = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

const Position* RiskManager::get_position(const Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(position_mutex_);
    auto it = positions_.find(symbol.str());
    return (it != positions_.end()) ? &it->second : nullptr;
}

Quantity RiskManager::get_position_qty(const Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(position_mutex_);
    auto it = positions_.find(symbol.str());
    return (it != positions_.end()) ? it->second.quantity : 0;
}

void RiskManager::update_mark_price(const Symbol& symbol, Price price) {
    std::lock_guard<std::mutex> lock(position_mutex_);

    auto it = positions_.find(symbol.str());
    if (it != positions_.end()) {
        calculate_unrealized_pnl(it->second, price);
    }
}

void RiskManager::calculate_unrealized_pnl(Position& pos, Price current_price) {
    if (pos.quantity == 0 || pos.avg_price == 0) {
        pos.unrealized_pnl = 0.0;
        return;
    }

    if (pos.quantity > 0) {
        // Long position
        pos.unrealized_pnl = from_qty(pos.quantity) *
            (from_price(current_price) - from_price(pos.avg_price));
    } else {
        // Short position
        pos.unrealized_pnl = from_qty(-pos.quantity) *
            (from_price(pos.avg_price) - from_price(current_price));
    }
}

double RiskManager::get_unrealized_pnl() const {
    std::lock_guard<std::mutex> lock(position_mutex_);

    double total = 0.0;
    for (const auto& [sym, pos] : positions_) {
        total += pos.unrealized_pnl;
    }
    return total;
}

double RiskManager::get_realized_pnl() const {
    std::lock_guard<std::mutex> lock(position_mutex_);

    double total = 0.0;
    for (const auto& [sym, pos] : positions_) {
        total += pos.realized_pnl;
    }
    return total;
}

double RiskManager::get_total_pnl() const {
    return get_realized_pnl() + get_unrealized_pnl();
}

double RiskManager::get_daily_pnl() const {
    return daily_realized_pnl_.load(std::memory_order_relaxed) + get_unrealized_pnl();
}

double RiskManager::get_total_exposure() const {
    std::lock_guard<std::mutex> lock(position_mutex_);

    double total = 0.0;
    for (const auto& [sym, pos] : positions_) {
        // Would need current price to calculate properly
        total += std::abs(from_qty(pos.quantity) * from_price(pos.avg_price));
    }
    return total;
}

double RiskManager::get_net_exposure() const {
    std::lock_guard<std::mutex> lock(position_mutex_);

    double net = 0.0;
    for (const auto& [sym, pos] : positions_) {
        net += from_qty(pos.quantity) * from_price(pos.avg_price);
    }
    return net;
}

void RiskManager::activate_kill_switch(const std::string& reason) {
    if (!kill_switch_active_.exchange(true)) {
        if (kill_switch_callback_) {
            kill_switch_callback_(reason);
        }
    }
}

void RiskManager::deactivate_kill_switch() {
    kill_switch_active_ = false;
    error_count_ = 0;
    reject_count_ = 0;
}

bool RiskManager::is_kill_switch_active() const noexcept {
    return kill_switch_active_.load(std::memory_order_relaxed);
}

void RiskManager::set_kill_switch_callback(KillSwitchCallback cb) {
    kill_switch_callback_ = std::move(cb);
}

void RiskManager::enable_symbol(const Symbol& symbol) {
    symbol_enabled_[symbol.str()] = true;
}

void RiskManager::disable_symbol(const Symbol& symbol) {
    symbol_enabled_[symbol.str()] = false;
}

bool RiskManager::is_symbol_enabled(const Symbol& symbol) const {
    auto it = symbol_enabled_.find(symbol.str());
    return (it == symbol_enabled_.end()) ? true : it->second;  // Default enabled
}

void RiskManager::update_limits(const RiskLimits& limits) {
    limits_ = limits;
}

void RiskManager::reset_daily_stats() {
    daily_realized_pnl_ = 0.0;
    peak_equity_.store(get_total_pnl(), std::memory_order_relaxed);
    error_count_ = 0;
    reject_count_ = 0;
}

} // namespace hft
