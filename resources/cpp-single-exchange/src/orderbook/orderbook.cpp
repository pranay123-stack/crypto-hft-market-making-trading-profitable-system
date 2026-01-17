#include "orderbook/orderbook.hpp"
#include <algorithm>
#include <numeric>

namespace hft {

// ============================================================================
// OrderBook Implementation
// ============================================================================

OrderBook::OrderBook(const Symbol& symbol)
    : symbol_(symbol)
{
    bid_cache_.fill(PriceLevel{});
    ask_cache_.fill(PriceLevel{});
}

void OrderBook::update_bid(Price price, Quantity quantity) {
    if (quantity == 0) {
        bids_.erase(price);
    } else {
        bids_[price] = PriceLevel(price, quantity);
    }
    bid_cache_dirty_ = true;
    last_update_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void OrderBook::update_ask(Price price, Quantity quantity) {
    if (quantity == 0) {
        asks_.erase(price);
    } else {
        asks_[price] = PriceLevel(price, quantity);
    }
    ask_cache_dirty_ = true;
    last_update_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void OrderBook::clear_bids() {
    bids_.clear();
    bid_cache_size_ = 0;
    bid_cache_dirty_ = true;
}

void OrderBook::clear_asks() {
    asks_.clear();
    ask_cache_size_ = 0;
    ask_cache_dirty_ = true;
}

void OrderBook::add_order(const Order& order) {
    orders_[order.id] = order;

    auto& levels = (order.side == Side::BUY) ? bids_ : asks_;
    auto it = levels.find(order.price);

    if (it != levels.end()) {
        it->second.quantity += order.quantity;
        it->second.order_count++;
    } else {
        levels[order.price] = PriceLevel(order.price, order.quantity, 1);
    }

    if (order.side == Side::BUY) {
        bid_cache_dirty_ = true;
    } else {
        ask_cache_dirty_ = true;
    }
}

void OrderBook::modify_order(OrderId id, Quantity new_qty) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;

    Order& order = it->second;
    Quantity diff = new_qty - order.quantity;

    auto& levels = (order.side == Side::BUY) ? bids_ : asks_;
    auto level_it = levels.find(order.price);

    if (level_it != levels.end()) {
        level_it->second.quantity += diff;
        if (level_it->second.quantity <= 0) {
            levels.erase(level_it);
        }
    }

    order.quantity = new_qty;

    if (order.side == Side::BUY) {
        bid_cache_dirty_ = true;
    } else {
        ask_cache_dirty_ = true;
    }
}

void OrderBook::remove_order(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;

    Order& order = it->second;
    auto& levels = (order.side == Side::BUY) ? bids_ : asks_;
    auto level_it = levels.find(order.price);

    if (level_it != levels.end()) {
        level_it->second.quantity -= order.remaining();
        level_it->second.order_count--;
        if (level_it->second.quantity <= 0 || level_it->second.order_count == 0) {
            levels.erase(level_it);
        }
    }

    if (order.side == Side::BUY) {
        bid_cache_dirty_ = true;
    } else {
        ask_cache_dirty_ = true;
    }

    orders_.erase(it);
}

void OrderBook::apply_snapshot(const std::vector<PriceLevel>& bids,
                               const std::vector<PriceLevel>& asks) {
    bids_.clear();
    asks_.clear();

    for (const auto& level : bids) {
        bids_[level.price] = level;
    }

    for (const auto& level : asks) {
        asks_[level.price] = level;
    }

    bid_cache_dirty_ = true;
    ask_cache_dirty_ = true;
    last_update_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Quantity> OrderBook::best_bid_qty() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->second.quantity;
}

std::optional<Quantity> OrderBook::best_ask_qty() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->second.quantity;
}

Price OrderBook::mid_price() const noexcept {
    auto bid = best_bid();
    auto ask = best_ask();
    if (!bid || !ask) return 0;
    return (*bid + *ask) / 2;
}

Price OrderBook::spread() const noexcept {
    auto bid = best_bid();
    auto ask = best_ask();
    if (!bid || !ask) return 0;
    return *ask - *bid;
}

double OrderBook::spread_bps() const noexcept {
    Price mid = mid_price();
    if (mid == 0) return 0.0;
    return 10000.0 * static_cast<double>(spread()) / static_cast<double>(mid);
}

const PriceLevel* OrderBook::get_bid_level(size_t depth) const noexcept {
    if (bid_cache_dirty_) {
        const_cast<OrderBook*>(this)->rebuild_bid_cache();
    }
    if (depth >= bid_cache_size_) return nullptr;
    return &bid_cache_[depth];
}

const PriceLevel* OrderBook::get_ask_level(size_t depth) const noexcept {
    if (ask_cache_dirty_) {
        const_cast<OrderBook*>(this)->rebuild_ask_cache();
    }
    if (depth >= ask_cache_size_) return nullptr;
    return &ask_cache_[depth];
}

size_t OrderBook::bid_depth() const noexcept {
    if (bid_cache_dirty_) {
        const_cast<OrderBook*>(this)->rebuild_bid_cache();
    }
    return bid_cache_size_;
}

size_t OrderBook::ask_depth() const noexcept {
    if (ask_cache_dirty_) {
        const_cast<OrderBook*>(this)->rebuild_ask_cache();
    }
    return ask_cache_size_;
}

Price OrderBook::vwap_bid(Quantity qty) const {
    Quantity remaining = qty;
    int64_t total_value = 0;
    Quantity total_qty = 0;

    for (const auto& [price, level] : bids_) {
        Quantity fill = std::min(remaining, level.quantity);
        total_value += price * fill;
        total_qty += fill;
        remaining -= fill;
        if (remaining <= 0) break;
    }

    if (total_qty == 0) return 0;
    return total_value / total_qty;
}

Price OrderBook::vwap_ask(Quantity qty) const {
    Quantity remaining = qty;
    int64_t total_value = 0;
    Quantity total_qty = 0;

    for (const auto& [price, level] : asks_) {
        Quantity fill = std::min(remaining, level.quantity);
        total_value += price * fill;
        total_qty += fill;
        remaining -= fill;
        if (remaining <= 0) break;
    }

    if (total_qty == 0) return 0;
    return total_value / total_qty;
}

double OrderBook::book_imbalance() const noexcept {
    return book_imbalance(1);
}

double OrderBook::book_imbalance(size_t levels) const noexcept {
    Quantity bid_vol = 0;
    Quantity ask_vol = 0;

    size_t i = 0;
    for (const auto& [price, level] : bids_) {
        if (i++ >= levels) break;
        bid_vol += level.quantity;
    }

    i = 0;
    for (const auto& [price, level] : asks_) {
        if (i++ >= levels) break;
        ask_vol += level.quantity;
    }

    Quantity total = bid_vol + ask_vol;
    if (total == 0) return 0.0;

    return static_cast<double>(bid_vol - ask_vol) / static_cast<double>(total);
}

bool OrderBook::is_valid() const noexcept {
    if (bids_.empty() || asks_.empty()) return false;

    auto bid = best_bid();
    auto ask = best_ask();

    // Crossed book is invalid
    if (bid && ask && *bid >= *ask) return false;

    return true;
}

void OrderBook::rebuild_bid_cache() {
    bid_cache_size_ = 0;
    for (const auto& [price, level] : bids_) {
        if (bid_cache_size_ >= MAX_DEPTH) break;
        bid_cache_[bid_cache_size_++] = level;
    }
    bid_cache_dirty_ = false;
}

void OrderBook::rebuild_ask_cache() {
    ask_cache_size_ = 0;
    for (const auto& [price, level] : asks_) {
        if (ask_cache_size_ >= MAX_DEPTH) break;
        ask_cache_[ask_cache_size_++] = level;
    }
    ask_cache_dirty_ = false;
}

// ============================================================================
// OrderBookManager Implementation
// ============================================================================

OrderBook& OrderBookManager::get_or_create(const Symbol& symbol) {
    std::string key = symbol.str();
    auto it = books_.find(key);
    if (it != books_.end()) {
        return *it->second;
    }
    auto [new_it, inserted] = books_.emplace(key, std::make_unique<OrderBook>(symbol));
    return *new_it->second;
}

OrderBook* OrderBookManager::get(const Symbol& symbol) {
    auto it = books_.find(symbol.str());
    return (it != books_.end()) ? it->second.get() : nullptr;
}

const OrderBook* OrderBookManager::get(const Symbol& symbol) const {
    auto it = books_.find(symbol.str());
    return (it != books_.end()) ? it->second.get() : nullptr;
}

void OrderBookManager::remove(const Symbol& symbol) {
    books_.erase(symbol.str());
}

void OrderBookManager::clear() {
    books_.clear();
}

} // namespace hft
