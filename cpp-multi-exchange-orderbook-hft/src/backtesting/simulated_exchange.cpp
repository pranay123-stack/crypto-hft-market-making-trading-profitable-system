/**
 * @file simulated_exchange.cpp
 * @brief Simulated exchange implementation for backtesting
 */

#include "backtesting/simulated_exchange.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace hft::backtesting {

// ============================================================================
// FeeConfig Implementation
// ============================================================================

double FeeConfig::getMakerFee(double volume_30d) const {
    if (tiers.empty()) {
        return maker_fee;
    }

    // Find applicable tier based on 30-day volume
    double fee = maker_fee;
    for (const auto& tier : tiers) {
        if (volume_30d >= tier.min_volume) {
            fee = tier.maker_fee;
        } else {
            break;
        }
    }
    return fee;
}

double FeeConfig::getTakerFee(double volume_30d) const {
    if (tiers.empty()) {
        return taker_fee;
    }

    double fee = taker_fee;
    for (const auto& tier : tiers) {
        if (volume_30d >= tier.min_volume) {
            fee = tier.taker_fee;
        } else {
            break;
        }
    }
    return fee;
}

// ============================================================================
// SimulatedOrderBook Implementation
// ============================================================================

SimulatedOrderBook::SimulatedOrderBook(const core::Symbol& symbol)
    : symbol_(symbol) {
}

void SimulatedOrderBook::applySnapshot(const OrderBookSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_snapshot_ = snapshot;
}

void SimulatedOrderBook::applyUpdate(const OrderBookUpdate& update) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (update.side == core::Side::Buy) {
        // Update bid side
        for (uint8_t i = 0; i < current_snapshot_.bid_count; ++i) {
            if (current_snapshot_.bids[i].price == update.price) {
                if (update.quantity.is_zero()) {
                    // Delete level - shift remaining levels
                    for (uint8_t j = i; j < current_snapshot_.bid_count - 1; ++j) {
                        current_snapshot_.bids[j] = current_snapshot_.bids[j + 1];
                    }
                    current_snapshot_.bid_count--;
                } else {
                    current_snapshot_.bids[i].quantity = update.quantity;
                }
                return;
            }
        }

        // Level not found - insert new level if not zero quantity
        if (!update.quantity.is_zero() &&
            current_snapshot_.bid_count < OrderBookSnapshot::MAX_DEPTH) {

            // Find insertion point (bids sorted descending)
            uint8_t insert_pos = 0;
            while (insert_pos < current_snapshot_.bid_count &&
                   current_snapshot_.bids[insert_pos].price > update.price) {
                insert_pos++;
            }

            // Shift levels down
            for (uint8_t j = current_snapshot_.bid_count; j > insert_pos; --j) {
                current_snapshot_.bids[j] = current_snapshot_.bids[j - 1];
            }

            current_snapshot_.bids[insert_pos].price = update.price;
            current_snapshot_.bids[insert_pos].quantity = update.quantity;
            current_snapshot_.bid_count++;
        }
    } else {
        // Update ask side
        for (uint8_t i = 0; i < current_snapshot_.ask_count; ++i) {
            if (current_snapshot_.asks[i].price == update.price) {
                if (update.quantity.is_zero()) {
                    for (uint8_t j = i; j < current_snapshot_.ask_count - 1; ++j) {
                        current_snapshot_.asks[j] = current_snapshot_.asks[j + 1];
                    }
                    current_snapshot_.ask_count--;
                } else {
                    current_snapshot_.asks[i].quantity = update.quantity;
                }
                return;
            }
        }

        if (!update.quantity.is_zero() &&
            current_snapshot_.ask_count < OrderBookSnapshot::MAX_DEPTH) {

            // Find insertion point (asks sorted ascending)
            uint8_t insert_pos = 0;
            while (insert_pos < current_snapshot_.ask_count &&
                   current_snapshot_.asks[insert_pos].price < update.price) {
                insert_pos++;
            }

            for (uint8_t j = current_snapshot_.ask_count; j > insert_pos; --j) {
                current_snapshot_.asks[j] = current_snapshot_.asks[j - 1];
            }

            current_snapshot_.asks[insert_pos].price = update.price;
            current_snapshot_.asks[insert_pos].quantity = update.quantity;
            current_snapshot_.ask_count++;
        }
    }

    current_snapshot_.timestamp = update.timestamp;
    current_snapshot_.sequence_number++;
}

SimulatedOrderBook::MatchResult SimulatedOrderBook::matchMarketOrder(
    const SimulatedOrder& order,
    core::Timestamp timestamp,
    const SlippageConfig& slippage) {

    std::lock_guard<std::mutex> lock(mutex_);

    MatchResult result;
    result.remaining_quantity = order.remaining_quantity;

    // Determine which side of the book to take liquidity from
    const auto& levels = (order.side == core::Side::Buy)
        ? current_snapshot_.asks
        : current_snapshot_.bids;
    const uint8_t level_count = (order.side == core::Side::Buy)
        ? current_snapshot_.ask_count
        : current_snapshot_.bid_count;

    if (level_count == 0) {
        result.rejected = true;
        result.reject_reason = "No liquidity available";
        return result;
    }

    // Calculate slippage
    core::Quantity slippage_qty = calculateSlippage(
        order.remaining_quantity, order.side, slippage);

    // Walk through price levels filling the order
    for (uint8_t i = 0; i < level_count && result.remaining_quantity > core::Quantity{0}; ++i) {
        core::Quantity available = levels[i].quantity;

        // Apply liquidity factor
        if (slippage.model == SlippageConfig::Model::OrderBook) {
            available = core::Quantity{
                static_cast<uint64_t>(available.value * slippage.liquidity_factor)
            };
        }

        core::Quantity fill_qty = std::min(result.remaining_quantity, available);

        if (fill_qty > core::Quantity{0}) {
            Fill fill;
            fill.order_id = order.order_id;
            fill.trade_id = core::TradeId{next_trade_id_++};
            fill.exchange = order.exchange;
            fill.symbol = order.symbol;
            fill.side = order.side;
            fill.price = levels[i].price;
            fill.quantity = fill_qty;
            fill.timestamp = timestamp;
            fill.is_maker = false;

            result.fills.push_back(fill);
            result.remaining_quantity = result.remaining_quantity - fill_qty;
        }
    }

    return result;
}

SimulatedOrderBook::MatchResult SimulatedOrderBook::matchLimitOrder(
    const SimulatedOrder& order,
    core::Timestamp timestamp,
    const SlippageConfig& slippage) {

    std::lock_guard<std::mutex> lock(mutex_);

    MatchResult result;
    result.remaining_quantity = order.remaining_quantity;

    // Check if order would cross the spread
    if (order.side == core::Side::Buy) {
        if (current_snapshot_.ask_count > 0 &&
            order.price >= current_snapshot_.asks[0].price) {
            result.would_cross_spread = true;

            // If post-only, reject
            if (order.is_post_only) {
                result.rejected = true;
                result.reject_reason = "Post-only order would cross spread";
                return result;
            }

            // Otherwise, fill at available prices up to limit
            for (uint8_t i = 0; i < current_snapshot_.ask_count &&
                 result.remaining_quantity > core::Quantity{0}; ++i) {

                if (current_snapshot_.asks[i].price > order.price) {
                    break;  // Beyond limit price
                }

                core::Quantity available = current_snapshot_.asks[i].quantity;
                if (slippage.model == SlippageConfig::Model::OrderBook) {
                    available = core::Quantity{
                        static_cast<uint64_t>(available.value * slippage.liquidity_factor)
                    };
                }

                core::Quantity fill_qty = std::min(result.remaining_quantity, available);

                if (fill_qty > core::Quantity{0}) {
                    Fill fill;
                    fill.order_id = order.order_id;
                    fill.trade_id = core::TradeId{next_trade_id_++};
                    fill.exchange = order.exchange;
                    fill.symbol = order.symbol;
                    fill.side = order.side;
                    fill.price = current_snapshot_.asks[i].price;
                    fill.quantity = fill_qty;
                    fill.timestamp = timestamp;
                    fill.is_maker = false;

                    result.fills.push_back(fill);
                    result.remaining_quantity = result.remaining_quantity - fill_qty;
                }
            }
        }
    } else {  // Sell order
        if (current_snapshot_.bid_count > 0 &&
            order.price <= current_snapshot_.bids[0].price) {
            result.would_cross_spread = true;

            if (order.is_post_only) {
                result.rejected = true;
                result.reject_reason = "Post-only order would cross spread";
                return result;
            }

            for (uint8_t i = 0; i < current_snapshot_.bid_count &&
                 result.remaining_quantity > core::Quantity{0}; ++i) {

                if (current_snapshot_.bids[i].price < order.price) {
                    break;
                }

                core::Quantity available = current_snapshot_.bids[i].quantity;
                if (slippage.model == SlippageConfig::Model::OrderBook) {
                    available = core::Quantity{
                        static_cast<uint64_t>(available.value * slippage.liquidity_factor)
                    };
                }

                core::Quantity fill_qty = std::min(result.remaining_quantity, available);

                if (fill_qty > core::Quantity{0}) {
                    Fill fill;
                    fill.order_id = order.order_id;
                    fill.trade_id = core::TradeId{next_trade_id_++};
                    fill.exchange = order.exchange;
                    fill.symbol = order.symbol;
                    fill.side = order.side;
                    fill.price = current_snapshot_.bids[i].price;
                    fill.quantity = fill_qty;
                    fill.timestamp = timestamp;
                    fill.is_maker = false;

                    result.fills.push_back(fill);
                    result.remaining_quantity = result.remaining_quantity - fill_qty;
                }
            }
        }
    }

    return result;
}

void SimulatedOrderBook::insertOrder(const SimulatedOrder& order) {
    std::lock_guard<std::mutex> lock(mutex_);

    BookOrder book_order;
    book_order.order_id = order.order_id;
    book_order.price = order.price;
    book_order.quantity = order.remaining_quantity;
    book_order.time = order.created_time;

    if (order.side == core::Side::Buy) {
        bid_orders_[order.price].push_back(book_order);
    } else {
        ask_orders_[order.price].push_back(book_order);
    }
}

void SimulatedOrderBook::removeOrder(core::OrderId order_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Search bid orders
    for (auto& [price, orders] : bid_orders_) {
        auto it = std::find_if(orders.begin(), orders.end(),
            [order_id](const BookOrder& o) {
                return o.order_id == order_id;
            });
        if (it != orders.end()) {
            orders.erase(it);
            if (orders.empty()) {
                bid_orders_.erase(price);
            }
            return;
        }
    }

    // Search ask orders
    for (auto& [price, orders] : ask_orders_) {
        auto it = std::find_if(orders.begin(), orders.end(),
            [order_id](const BookOrder& o) {
                return o.order_id == order_id;
            });
        if (it != orders.end()) {
            orders.erase(it);
            if (orders.empty()) {
                ask_orders_.erase(price);
            }
            return;
        }
    }
}

core::Price SimulatedOrderBook::bestBid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_snapshot_.bid_count > 0) {
        return current_snapshot_.bids[0].price;
    }
    return core::Price{0};
}

core::Price SimulatedOrderBook::bestAsk() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_snapshot_.ask_count > 0) {
        return current_snapshot_.asks[0].price;
    }
    return core::Price{0};
}

core::Price SimulatedOrderBook::midPrice() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_snapshot_.mid_price();
}

double SimulatedOrderBook::spreadBps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_snapshot_.spread_bps();
}

core::Quantity SimulatedOrderBook::bidQuantityAtPrice(core::Price price) const {
    std::lock_guard<std::mutex> lock(mutex_);

    for (uint8_t i = 0; i < current_snapshot_.bid_count; ++i) {
        if (current_snapshot_.bids[i].price == price) {
            return current_snapshot_.bids[i].quantity;
        }
    }
    return core::Quantity{0};
}

core::Quantity SimulatedOrderBook::askQuantityAtPrice(core::Price price) const {
    std::lock_guard<std::mutex> lock(mutex_);

    for (uint8_t i = 0; i < current_snapshot_.ask_count; ++i) {
        if (current_snapshot_.asks[i].price == price) {
            return current_snapshot_.asks[i].quantity;
        }
    }
    return core::Quantity{0};
}

core::Quantity SimulatedOrderBook::totalBidQuantity(uint32_t levels) const {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t total = 0;
    uint32_t count = std::min(static_cast<uint32_t>(current_snapshot_.bid_count), levels);

    for (uint32_t i = 0; i < count; ++i) {
        total += current_snapshot_.bids[i].quantity.value;
    }

    return core::Quantity{total};
}

core::Quantity SimulatedOrderBook::totalAskQuantity(uint32_t levels) const {
    std::lock_guard<std::mutex> lock(mutex_);

    uint64_t total = 0;
    uint32_t count = std::min(static_cast<uint32_t>(current_snapshot_.ask_count), levels);

    for (uint32_t i = 0; i < count; ++i) {
        total += current_snapshot_.asks[i].quantity.value;
    }

    return core::Quantity{total};
}

core::Quantity SimulatedOrderBook::calculateSlippage(
    core::Quantity order_size,
    core::Side side,
    const SlippageConfig& config) const {

    if (config.model == SlippageConfig::Model::None) {
        return core::Quantity{0};
    }

    double size = order_size.to_double();
    double slippage_pct = 0.0;

    switch (config.model) {
        case SlippageConfig::Model::Fixed:
            slippage_pct = config.fixed_slippage_bps / 10000.0;
            break;

        case SlippageConfig::Model::Linear: {
            // Estimate ADV from total book quantity
            double adv = (totalBidQuantity(20).to_double() + totalAskQuantity(20).to_double()) * 100;
            if (adv > 0) {
                slippage_pct = (config.linear_base_bps +
                               config.linear_coefficient * (size / adv)) / 10000.0;
            }
            break;
        }

        case SlippageConfig::Model::SquareRoot: {
            double adv = (totalBidQuantity(20).to_double() + totalAskQuantity(20).to_double()) * 100;
            if (adv > 0) {
                slippage_pct = config.sqrt_coefficient * std::sqrt(size / adv);
            }
            break;
        }

        case SlippageConfig::Model::OrderBook:
            // Slippage is calculated during actual matching
            return core::Quantity{0};

        default:
            break;
    }

    return core::Quantity::from_double(size * slippage_pct);
}

// ============================================================================
// SimulatedExchange Implementation
// ============================================================================

SimulatedExchange::SimulatedExchange(const SimulatedExchangeConfig& config)
    : config_(config)
    , rng_(std::random_device{}())
    , latency_dist_(0.0, 1.0) {
}

SimulatedExchange::~SimulatedExchange() = default;

void SimulatedExchange::initialize(
    const std::unordered_map<std::string, double>& initial_balances) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Reset state
    orders_.clear();
    order_history_.clear();
    fill_history_.clear();
    positions_.clear();
    balances_.clear();
    order_books_.clear();
    last_prices_.clear();

    while (!pending_events_.empty()) {
        pending_events_.pop();
    }

    order_timestamps_.clear();
    cancel_timestamps_.clear();

    next_order_id_ = 1;
    total_orders_ = 0;
    total_fills_ = 0;
    total_rejects_ = 0;
    total_fees_paid_ = 0.0;
    total_volume_ = 0.0;

    current_time_ = core::Timestamp::zero();

    // Initialize balances
    for (const auto& [currency, amount] : initial_balances) {
        AccountBalance balance;
        balance.currency = currency;
        balance.total = amount;
        balance.available = amount;
        balance.locked = 0.0;
        balances_[currency] = balance;
    }
}

void SimulatedExchange::reset() {
    std::unordered_map<std::string, double> initial_balances;
    for (const auto& [currency, balance] : balances_) {
        initial_balances[currency] = balance.total;
    }
    initialize(initial_balances);
}

void SimulatedExchange::setCurrentTime(core::Timestamp time) {
    current_time_ = time;
    processEvents();
}

void SimulatedExchange::onOrderBookSnapshot(const OrderBookSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key(snapshot.symbol.view());

    auto it = order_books_.find(key);
    if (it == order_books_.end()) {
        order_books_[key] = std::make_unique<SimulatedOrderBook>(snapshot.symbol);
    }

    order_books_[key]->applySnapshot(snapshot);

    // Update last price to mid price
    auto mid = snapshot.mid_price();
    if (!mid.is_zero()) {
        last_prices_[key] = mid;
    }
}

void SimulatedExchange::onOrderBookUpdate(const OrderBookUpdate& update) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key(update.symbol.view());

    auto it = order_books_.find(key);
    if (it != order_books_.end()) {
        it->second->applyUpdate(update);

        // Update last price
        auto mid = it->second->midPrice();
        if (!mid.is_zero()) {
            last_prices_[key] = mid;
        }
    }
}

void SimulatedExchange::onTrade(const Trade& trade) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key(trade.symbol.view());
    last_prices_[key] = trade.price;

    // Check if any stop orders should trigger
    for (auto& [order_id, order] : orders_) {
        if (order.symbol == trade.symbol && order.is_active()) {
            bool trigger = false;

            if (order.type == core::OrderType::StopLoss ||
                order.type == core::OrderType::StopLossLimit) {
                // Stop loss triggers when price falls below stop
                if (order.side == core::Side::Sell && trade.price <= order.stop_price) {
                    trigger = true;
                }
                // Stop loss for short triggers when price rises above stop
                if (order.side == core::Side::Buy && trade.price >= order.stop_price) {
                    trigger = true;
                }
            } else if (order.type == core::OrderType::TakeProfit ||
                       order.type == core::OrderType::TakeProfitLimit) {
                // Take profit for long triggers when price rises above take profit
                if (order.side == core::Side::Sell && trade.price >= order.stop_price) {
                    trigger = true;
                }
                // Take profit for short triggers when price falls below take profit
                if (order.side == core::Side::Buy && trade.price <= order.stop_price) {
                    trigger = true;
                }
            }

            if (trigger) {
                executeStopOrder(order);
            }
        }
    }

    // Update unrealized PnL for positions
    auto pos_it = positions_.find(key);
    if (pos_it != positions_.end()) {
        auto& pos = pos_it->second;
        if (!pos.is_flat()) {
            double current_price = trade.price.to_double();
            pos.unrealized_pnl = (current_price - pos.average_entry_price) * pos.quantity;
            pos.updated_time = trade.timestamp;
        }
    }
}

core::OrderId SimulatedExchange::submitOrder(
    const core::Symbol& symbol,
    core::Side side,
    core::OrderType type,
    core::Quantity quantity,
    core::Price price,
    core::Price stop_price,
    bool post_only,
    bool reduce_only,
    core::OrderId client_order_id) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Check rate limit
    if (!checkRateLimit()) {
        emitError(core::ErrorCode::RateLimited, "Order rate limit exceeded");
        return core::OrderId::invalid();
    }

    // Create order
    SimulatedOrder order;
    order.order_id = core::OrderId{next_order_id_++};
    order.client_order_id = client_order_id.is_valid() ? client_order_id : order.order_id;
    order.exchange = config_.exchange;
    order.symbol = symbol;
    order.type = type;
    order.side = side;
    order.price = price;
    order.stop_price = stop_price;
    order.original_quantity = quantity;
    order.filled_quantity = core::Quantity{0};
    order.remaining_quantity = quantity;
    order.status = core::OrderStatus::Pending;
    order.created_time = current_time_;
    order.updated_time = current_time_;
    order.is_post_only = post_only;
    order.reduce_only = reduce_only;

    // Validate order
    std::string error;
    if (!validateOrder(order, error)) {
        total_rejects_++;
        emitError(core::ErrorCode::OrderRejected, error);
        return core::OrderId::invalid();
    }

    // Random rejection simulation
    if (config_.reject_probability > 0.0) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng_) < config_.reject_probability) {
            total_rejects_++;
            emitError(core::ErrorCode::OrderRejected, "Random rejection (simulation)");
            return core::OrderId::invalid();
        }
    }

    // Calculate required margin/balance and lock it
    double notional = price.to_double() * quantity.to_double();
    if (side == core::Side::Buy) {
        std::string quote_currency = "USDT";  // Assume USDT quote
        lockBalance(quote_currency, notional);
    } else {
        std::string base_currency(symbol.view().substr(0, symbol.view().find("USDT")));
        if (!base_currency.empty()) {
            lockBalance(base_currency, quantity.to_double());
        }
    }

    // Add to orders map
    orders_[order.order_id.value] = order;
    total_orders_++;

    // Record timestamp for rate limiting
    order_timestamps_.push_back(current_time_);
    while (!order_timestamps_.empty() &&
           order_timestamps_.front().nanos + 1'000'000'000ULL < current_time_.nanos) {
        order_timestamps_.pop_front();
    }

    // Schedule order processing with latency
    core::Timestamp process_time = simulateLatency(
        config_.latency.order_submit_mean_us,
        config_.latency.order_submit_stddev_us,
        config_.latency.order_submit_min_us,
        config_.latency.order_submit_max_us);

    order.visible_time = process_time;

    pending_events_.push({process_time, [this, order_id = order.order_id]() {
        auto it = orders_.find(order_id.value);
        if (it != orders_.end()) {
            processOrder(it->second);
        }
    }});

    return order.order_id;
}

bool SimulatedExchange::cancelOrder(core::OrderId order_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = orders_.find(order_id.value);
    if (it == orders_.end()) {
        emitError(core::ErrorCode::OrderNotFound, "Order not found");
        return false;
    }

    auto& order = it->second;
    if (!order.is_active()) {
        emitError(core::ErrorCode::OrderAlreadyCancelled, "Order not active");
        return false;
    }

    // Schedule cancellation with latency
    core::Timestamp cancel_time = simulateLatency(
        config_.latency.cancel_mean_us,
        config_.latency.cancel_stddev_us,
        config_.latency.cancel_min_us,
        config_.latency.cancel_max_us);

    pending_events_.push({cancel_time, [this, order_id]() {
        auto it = orders_.find(order_id.value);
        if (it != orders_.end() && it->second.is_active()) {
            auto& order = it->second;
            order.status = core::OrderStatus::Cancelled;
            order.updated_time = current_time_;

            // Remove from order book if passive
            std::string key(order.symbol.view());
            auto book_it = order_books_.find(key);
            if (book_it != order_books_.end()) {
                book_it->second->removeOrder(order_id);
            }

            // Unlock balance
            double locked_amount = order.price.to_double() * order.remaining_quantity.to_double();
            if (order.side == core::Side::Buy) {
                unlockBalance("USDT", locked_amount);
            } else {
                std::string base(order.symbol.view().substr(0, order.symbol.view().find("USDT")));
                if (!base.empty()) {
                    unlockBalance(base, order.remaining_quantity.to_double());
                }
            }

            // Move to history
            order_history_.push_back(order);
            orders_.erase(order_id.value);

            emitOrderUpdate(order);
        }
    }});

    // Update status to pending cancel
    order.status = core::OrderStatus::PendingCancel;
    emitOrderUpdate(order);

    cancel_timestamps_.push_back(current_time_);
    while (!cancel_timestamps_.empty() &&
           cancel_timestamps_.front().nanos + 1'000'000'000ULL < current_time_.nanos) {
        cancel_timestamps_.pop_front();
    }

    return true;
}

bool SimulatedExchange::cancelAllOrders(const core::Symbol& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);

    bool any_cancelled = false;

    for (auto& [order_id, order] : orders_) {
        if (order.is_active()) {
            if (symbol.empty() || order.symbol == symbol) {
                // Directly cancel without unlocking mutex
                order.status = core::OrderStatus::Cancelled;
                order.updated_time = current_time_;

                std::string key(order.symbol.view());
                auto book_it = order_books_.find(key);
                if (book_it != order_books_.end()) {
                    book_it->second->removeOrder(core::OrderId{order_id});
                }

                double locked_amount = order.price.to_double() * order.remaining_quantity.to_double();
                if (order.side == core::Side::Buy) {
                    unlockBalance("USDT", locked_amount);
                }

                emitOrderUpdate(order);
                any_cancelled = true;
            }
        }
    }

    // Clean up cancelled orders
    for (auto it = orders_.begin(); it != orders_.end();) {
        if (it->second.status == core::OrderStatus::Cancelled) {
            order_history_.push_back(it->second);
            it = orders_.erase(it);
        } else {
            ++it;
        }
    }

    return any_cancelled;
}

bool SimulatedExchange::modifyOrder(
    core::OrderId order_id,
    core::Quantity new_quantity,
    core::Price new_price) {

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = orders_.find(order_id.value);
    if (it == orders_.end()) {
        emitError(core::ErrorCode::OrderNotFound, "Order not found");
        return false;
    }

    auto& order = it->second;
    if (!order.is_active()) {
        emitError(core::ErrorCode::OrderAlreadyCancelled, "Order not active");
        return false;
    }

    // Schedule modification with latency
    core::Timestamp modify_time = simulateLatency(
        config_.latency.order_submit_mean_us,
        config_.latency.order_submit_stddev_us,
        config_.latency.order_submit_min_us,
        config_.latency.order_submit_max_us);

    pending_events_.push({modify_time, [this, order_id, new_quantity, new_price]() {
        auto it = orders_.find(order_id.value);
        if (it != orders_.end() && it->second.is_active()) {
            auto& order = it->second;

            // Update balance lock
            double old_locked = order.price.to_double() * order.remaining_quantity.to_double();
            double new_locked = new_price.to_double() * new_quantity.to_double();

            if (order.side == core::Side::Buy) {
                if (new_locked > old_locked) {
                    lockBalance("USDT", new_locked - old_locked);
                } else {
                    unlockBalance("USDT", old_locked - new_locked);
                }
            }

            order.price = new_price;
            order.remaining_quantity = new_quantity;
            order.updated_time = current_time_;

            // Re-process the order (may get filled at new price)
            processOrder(order);
        }
    }});

    order.status = core::OrderStatus::PendingReplace;
    emitOrderUpdate(order);

    return true;
}

std::optional<SimulatedOrder> SimulatedExchange::getOrder(core::OrderId order_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = orders_.find(order_id.value);
    if (it != orders_.end()) {
        return it->second;
    }

    // Check history
    for (const auto& order : order_history_) {
        if (order.order_id == order_id) {
            return order;
        }
    }

    return std::nullopt;
}

std::vector<SimulatedOrder> SimulatedExchange::getOpenOrders(const core::Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<SimulatedOrder> result;

    for (const auto& [id, order] : orders_) {
        if (order.is_active()) {
            if (symbol.empty() || order.symbol == symbol) {
                result.push_back(order);
            }
        }
    }

    return result;
}

std::vector<SimulatedOrder> SimulatedExchange::getOrderHistory(
    const core::Symbol& symbol,
    size_t limit) const {

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<SimulatedOrder> result;

    for (auto it = order_history_.rbegin();
         it != order_history_.rend() && result.size() < limit;
         ++it) {
        if (symbol.empty() || it->symbol == symbol) {
            result.push_back(*it);
        }
    }

    return result;
}

std::vector<Fill> SimulatedExchange::getFills(
    const core::Symbol& symbol,
    size_t limit) const {

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Fill> result;

    for (auto it = fill_history_.rbegin();
         it != fill_history_.rend() && result.size() < limit;
         ++it) {
        if (symbol.empty() || it->symbol == symbol) {
            result.push_back(*it);
        }
    }

    return result;
}

Position SimulatedExchange::getPosition(const core::Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key(symbol.view());
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        return it->second;
    }

    Position empty_pos;
    empty_pos.symbol = symbol;
    return empty_pos;
}

std::vector<Position> SimulatedExchange::getAllPositions() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Position> result;
    for (const auto& [key, pos] : positions_) {
        if (!pos.is_flat()) {
            result.push_back(pos);
        }
    }

    return result;
}

AccountBalance SimulatedExchange::getBalance(const std::string& currency) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = balances_.find(currency);
    if (it != balances_.end()) {
        return it->second;
    }

    AccountBalance empty;
    empty.currency = currency;
    return empty;
}

std::unordered_map<std::string, AccountBalance> SimulatedExchange::getAllBalances() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return balances_;
}

double SimulatedExchange::getTotalEquity(const std::string& quote_currency) const {
    std::lock_guard<std::mutex> lock(mutex_);

    double equity = 0.0;

    // Add quote currency balance
    auto it = balances_.find(quote_currency);
    if (it != balances_.end()) {
        equity += it->second.total;
    }

    // Add unrealized PnL from positions
    for (const auto& [symbol, pos] : positions_) {
        equity += pos.unrealized_pnl;
    }

    return equity;
}

std::optional<OrderBookSnapshot> SimulatedExchange::getOrderBook(const core::Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key(symbol.view());
    auto it = order_books_.find(key);
    if (it != order_books_.end()) {
        return it->second->snapshot();
    }

    return std::nullopt;
}

core::Price SimulatedExchange::getLastPrice(const core::Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key(symbol.view());
    auto it = last_prices_.find(key);
    if (it != last_prices_.end()) {
        return it->second;
    }

    return core::Price{0};
}

core::Price SimulatedExchange::getMidPrice(const core::Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key(symbol.view());
    auto it = order_books_.find(key);
    if (it != order_books_.end()) {
        return it->second->midPrice();
    }

    return core::Price{0};
}

void SimulatedExchange::processEvents() {
    // Process all events up to current time
    while (!pending_events_.empty() &&
           pending_events_.top().execute_time <= current_time_) {
        auto event = pending_events_.top();
        pending_events_.pop();
        event.action();
    }
}

void SimulatedExchange::processOrder(SimulatedOrder& order) {
    if (order.status == core::OrderStatus::Cancelled ||
        order.status == core::OrderStatus::Rejected) {
        return;
    }

    order.status = core::OrderStatus::New;
    order.updated_time = current_time_;
    emitOrderUpdate(order);

    switch (order.type) {
        case core::OrderType::Market:
        case core::OrderType::IOC:
        case core::OrderType::FOK:
            executeMarketOrder(order);
            break;

        case core::OrderType::Limit:
        case core::OrderType::LimitMaker:
        case core::OrderType::GTC:
        case core::OrderType::PostOnly:
            executeLimitOrder(order);
            break;

        case core::OrderType::StopLoss:
        case core::OrderType::StopLossLimit:
        case core::OrderType::TakeProfit:
        case core::OrderType::TakeProfitLimit:
            // Stop orders wait for trigger
            break;

        default:
            break;
    }
}

void SimulatedExchange::executeMarketOrder(SimulatedOrder& order) {
    std::string key(order.symbol.view());
    auto book_it = order_books_.find(key);
    if (book_it == order_books_.end()) {
        order.status = core::OrderStatus::Rejected;
        emitError(core::ErrorCode::InvalidSymbol, "No order book for symbol");
        return;
    }

    auto result = book_it->second->matchMarketOrder(order, current_time_, config_.slippage);

    if (result.rejected) {
        order.status = core::OrderStatus::Rejected;
        total_rejects_++;
        emitError(core::ErrorCode::OrderRejected, result.reject_reason);
        emitOrderUpdate(order);
        return;
    }

    // Process fills
    for (auto& fill : result.fills) {
        fill.fee = calculateFee(fill);
        fill.fee_currency = config_.fees.fee_currency;

        order.filled_quantity = order.filled_quantity + fill.quantity;
        order.fill_count++;

        // Calculate weighted average fill price
        double total_value = order.average_fill_price.to_double() *
                            (order.filled_quantity.to_double() - fill.quantity.to_double()) +
                            fill.price.to_double() * fill.quantity.to_double();
        order.average_fill_price = core::Price::from_double(
            total_value / order.filled_quantity.to_double());

        order.total_fees += fill.fee;

        // Update position
        updatePosition(order.symbol, fill);

        // Update balance
        double notional = fill.notional();
        if (order.side == core::Side::Buy) {
            unlockBalance("USDT", notional);
            updateBalance("USDT", -(notional + fill.fee), true);
        } else {
            updateBalance("USDT", notional - fill.fee, true);
        }

        total_fees_paid_ += fill.fee;
        total_volume_ += notional;
        total_fills_++;

        fill_history_.push_back(fill);
        emitFill(fill);
    }

    order.remaining_quantity = result.remaining_quantity;
    order.updated_time = current_time_;

    if (order.remaining_quantity.is_zero()) {
        order.status = core::OrderStatus::Filled;
        order_history_.push_back(order);
        orders_.erase(order.order_id.value);
    } else if (order.filled_quantity > core::Quantity{0}) {
        // FOK orders must be fully filled
        if (order.type == core::OrderType::FOK) {
            // Rollback - this is simplified, real implementation would be more complex
            order.status = core::OrderStatus::Cancelled;
        } else if (order.type == core::OrderType::IOC) {
            // IOC cancels remaining
            order.status = core::OrderStatus::Cancelled;
            order_history_.push_back(order);
            orders_.erase(order.order_id.value);
        } else {
            order.status = core::OrderStatus::PartiallyFilled;
        }
    }

    emitOrderUpdate(order);
}

void SimulatedExchange::executeLimitOrder(SimulatedOrder& order) {
    std::string key(order.symbol.view());
    auto book_it = order_books_.find(key);
    if (book_it == order_books_.end()) {
        order.status = core::OrderStatus::Rejected;
        emitError(core::ErrorCode::InvalidSymbol, "No order book for symbol");
        return;
    }

    auto result = book_it->second->matchLimitOrder(order, current_time_, config_.slippage);

    if (result.rejected) {
        order.status = core::OrderStatus::Rejected;
        total_rejects_++;
        emitError(core::ErrorCode::OrderRejected, result.reject_reason);
        emitOrderUpdate(order);

        // Unlock balance
        double locked = order.price.to_double() * order.remaining_quantity.to_double();
        if (order.side == core::Side::Buy) {
            unlockBalance("USDT", locked);
        }

        return;
    }

    // Process any immediate fills
    for (auto& fill : result.fills) {
        fill.fee = calculateFee(fill);
        fill.fee_currency = config_.fees.fee_currency;

        order.filled_quantity = order.filled_quantity + fill.quantity;
        order.fill_count++;

        double total_value = order.average_fill_price.to_double() *
                            (order.filled_quantity.to_double() - fill.quantity.to_double()) +
                            fill.price.to_double() * fill.quantity.to_double();
        order.average_fill_price = core::Price::from_double(
            total_value / order.filled_quantity.to_double());

        order.total_fees += fill.fee;

        updatePosition(order.symbol, fill);

        double notional = fill.notional();
        if (order.side == core::Side::Buy) {
            unlockBalance("USDT", notional);
            updateBalance("USDT", -(notional + fill.fee), true);
        } else {
            updateBalance("USDT", notional - fill.fee, true);
        }

        total_fees_paid_ += fill.fee;
        total_volume_ += notional;
        total_fills_++;

        fill_history_.push_back(fill);
        emitFill(fill);
    }

    order.remaining_quantity = result.remaining_quantity;
    order.updated_time = current_time_;

    if (order.remaining_quantity.is_zero()) {
        order.status = core::OrderStatus::Filled;
        order_history_.push_back(order);
        orders_.erase(order.order_id.value);
    } else if (order.filled_quantity > core::Quantity{0}) {
        order.status = core::OrderStatus::PartiallyFilled;
        // Insert remaining into book
        book_it->second->insertOrder(order);
    } else {
        // No fills, insert into book as passive order
        book_it->second->insertOrder(order);
    }

    emitOrderUpdate(order);
}

void SimulatedExchange::executeStopOrder(SimulatedOrder& order) {
    // Convert to market or limit order and execute
    if (order.type == core::OrderType::StopLoss ||
        order.type == core::OrderType::TakeProfit) {
        order.type = core::OrderType::Market;
        executeMarketOrder(order);
    } else {
        // StopLossLimit or TakeProfitLimit
        order.type = core::OrderType::Limit;
        executeLimitOrder(order);
    }
}

void SimulatedExchange::updatePosition(const core::Symbol& symbol, const Fill& fill) {
    std::string key(symbol.view());

    auto it = positions_.find(key);
    if (it == positions_.end()) {
        Position pos;
        pos.symbol = symbol;
        pos.opened_time = fill.timestamp;
        positions_[key] = pos;
        it = positions_.find(key);
    }

    auto& pos = it->second;
    double fill_qty = fill.quantity.to_double();
    double fill_price = fill.price.to_double();

    if (fill.side == core::Side::Buy) {
        if (pos.quantity >= 0) {
            // Adding to long or opening long
            double old_value = pos.average_entry_price * pos.quantity;
            double new_value = fill_price * fill_qty;
            pos.quantity += fill_qty;
            pos.average_entry_price = (old_value + new_value) / pos.quantity;
        } else {
            // Closing short
            double closed_qty = std::min(fill_qty, -pos.quantity);
            double pnl = (pos.average_entry_price - fill_price) * closed_qty;
            pos.realized_pnl += pnl;
            pos.quantity += fill_qty;

            if (pos.quantity > 0) {
                pos.average_entry_price = fill_price;
            }
        }
    } else {  // Sell
        if (pos.quantity <= 0) {
            // Adding to short or opening short
            double old_value = pos.average_entry_price * (-pos.quantity);
            double new_value = fill_price * fill_qty;
            pos.quantity -= fill_qty;
            pos.average_entry_price = (old_value + new_value) / (-pos.quantity);
        } else {
            // Closing long
            double closed_qty = std::min(fill_qty, pos.quantity);
            double pnl = (fill_price - pos.average_entry_price) * closed_qty;
            pos.realized_pnl += pnl;
            pos.quantity -= fill_qty;

            if (pos.quantity < 0) {
                pos.average_entry_price = fill_price;
            }
        }
    }

    pos.total_fees += fill.fee;
    pos.updated_time = fill.timestamp;

    emitPositionUpdate(pos);
}

void SimulatedExchange::updateBalance(const std::string& currency, double delta, bool is_available) {
    auto it = balances_.find(currency);
    if (it == balances_.end()) {
        AccountBalance balance;
        balance.currency = currency;
        balances_[currency] = balance;
        it = balances_.find(currency);
    }

    it->second.total += delta;
    if (is_available) {
        it->second.available += delta;
    }

    emitBalanceUpdate(it->second);
}

void SimulatedExchange::lockBalance(const std::string& currency, double amount) {
    auto it = balances_.find(currency);
    if (it != balances_.end()) {
        it->second.available -= amount;
        it->second.locked += amount;
    }
}

void SimulatedExchange::unlockBalance(const std::string& currency, double amount) {
    auto it = balances_.find(currency);
    if (it != balances_.end()) {
        it->second.available += amount;
        it->second.locked -= amount;
    }
}

double SimulatedExchange::calculateFee(const Fill& fill) const {
    double rate = fill.is_maker
        ? config_.fees.getMakerFee()
        : config_.fees.getTakerFee();

    return fill.notional() * rate;
}

core::Timestamp SimulatedExchange::simulateLatency(
    uint64_t mean_us, uint64_t stddev_us,
    uint64_t min_us, uint64_t max_us) {

    double latency = mean_us + stddev_us * latency_dist_(rng_);

    // Clamp to valid range
    latency = std::max(static_cast<double>(min_us), latency);
    latency = std::min(static_cast<double>(max_us), latency);

    // Add jitter spike occasionally
    if (config_.latency.enable_jitter) {
        std::uniform_real_distribution<double> jitter_dist(0.0, 1.0);
        if (jitter_dist(rng_) < config_.latency.jitter_probability) {
            std::uniform_int_distribution<uint64_t> spike_dist(
                0, config_.latency.jitter_max_us);
            latency += spike_dist(rng_);
        }
    }

    return core::Timestamp{
        current_time_.nanos + static_cast<uint64_t>(latency) * 1000ULL
    };
}

bool SimulatedExchange::validateOrder(const SimulatedOrder& order, std::string& error) const {
    // Check minimum order size
    if (config_.min_order_size.value > 0 &&
        order.original_quantity < config_.min_order_size) {
        error = "Order quantity below minimum";
        return false;
    }

    // Check maximum order size
    if (config_.max_order_size.value > 0 &&
        order.original_quantity > config_.max_order_size) {
        error = "Order quantity above maximum";
        return false;
    }

    // Check sufficient balance
    if (order.side == core::Side::Buy) {
        double required = order.price.to_double() * order.original_quantity.to_double();
        auto it = balances_.find("USDT");
        if (it == balances_.end() || it->second.available < required) {
            error = "Insufficient balance";
            return false;
        }
    } else {
        std::string base(order.symbol.view().substr(0, order.symbol.view().find("USDT")));
        if (!base.empty()) {
            auto it = balances_.find(base);
            if (it != balances_.end() &&
                it->second.available < order.original_quantity.to_double()) {
                // Check if we have a position to sell
                std::string pos_key(order.symbol.view());
                auto pos_it = positions_.find(pos_key);
                if (pos_it == positions_.end() ||
                    pos_it->second.quantity < order.original_quantity.to_double()) {
                    // Allow shorting if not reduce_only
                    if (order.reduce_only) {
                        error = "Reduce-only order: insufficient position";
                        return false;
                    }
                }
            }
        }
    }

    // Check position limits
    if (config_.max_position_size > 0) {
        std::string key(order.symbol.view());
        auto pos_it = positions_.find(key);
        double current_pos = pos_it != positions_.end() ? pos_it->second.quantity : 0.0;
        double new_pos = current_pos;

        if (order.side == core::Side::Buy) {
            new_pos += order.original_quantity.to_double();
        } else {
            new_pos -= order.original_quantity.to_double();
        }

        if (std::abs(new_pos) > config_.max_position_size) {
            error = "Position limit exceeded";
            return false;
        }
    }

    // Check reduce-only constraint
    if (order.reduce_only) {
        std::string key(order.symbol.view());
        auto pos_it = positions_.find(key);
        if (pos_it == positions_.end() || pos_it->second.is_flat()) {
            error = "Reduce-only order: no position to reduce";
            return false;
        }

        // Check that order reduces position
        if ((pos_it->second.is_long() && order.side == core::Side::Buy) ||
            (pos_it->second.is_short() && order.side == core::Side::Sell)) {
            error = "Reduce-only order would increase position";
            return false;
        }
    }

    return true;
}

bool SimulatedExchange::checkRateLimit() {
    // Check orders per second
    if (order_timestamps_.size() >= config_.max_orders_per_second) {
        return false;
    }
    return true;
}

bool SimulatedExchange::checkPositionLimit(
    const core::Symbol& symbol, double additional_size) const {

    if (config_.max_position_size == 0.0) return true;

    std::string key(symbol.view());
    auto it = positions_.find(key);
    double current = it != positions_.end() ? std::abs(it->second.quantity) : 0.0;

    return (current + additional_size) <= config_.max_position_size;
}

void SimulatedExchange::emitOrderUpdate(const SimulatedOrder& order) {
    if (on_order_) {
        on_order_(order);
    }
}

void SimulatedExchange::emitFill(const Fill& fill) {
    if (on_fill_) {
        on_fill_(fill);
    }
}

void SimulatedExchange::emitPositionUpdate(const Position& position) {
    if (on_position_) {
        on_position_(position);
    }
}

void SimulatedExchange::emitBalanceUpdate(const AccountBalance& balance) {
    if (on_balance_) {
        on_balance_(balance);
    }
}

void SimulatedExchange::emitError(core::ErrorCode code, const std::string& message) {
    if (on_error_) {
        on_error_(code, message);
    }
}

// ============================================================================
// SimulatedExchangeFactory Implementation
// ============================================================================

std::unique_ptr<SimulatedExchange> SimulatedExchangeFactory::create(core::Exchange exchange) {
    auto config = getDefaultConfig(exchange);
    return std::make_unique<SimulatedExchange>(config);
}

std::unique_ptr<SimulatedExchange> SimulatedExchangeFactory::create(
    const SimulatedExchangeConfig& config) {
    return std::make_unique<SimulatedExchange>(config);
}

SimulatedExchangeConfig SimulatedExchangeFactory::getDefaultConfig(core::Exchange exchange) {
    SimulatedExchangeConfig config;
    config.exchange = exchange;

    switch (exchange) {
        case core::Exchange::Binance:
            config.name = "Binance";
            config.fees.maker_fee = 0.0001;   // 0.01%
            config.fees.taker_fee = 0.0001;   // 0.01%
            config.latency.order_submit_mean_us = 300;
            config.latency.cancel_mean_us = 200;
            config.latency.market_data_mean_us = 50;
            config.min_order_size = core::Quantity::from_double(0.00001);
            break;

        case core::Exchange::Coinbase:
            config.name = "Coinbase";
            config.fees.maker_fee = 0.004;    // 0.4%
            config.fees.taker_fee = 0.006;    // 0.6%
            config.latency.order_submit_mean_us = 500;
            config.latency.cancel_mean_us = 400;
            config.latency.market_data_mean_us = 100;
            config.min_order_size = core::Quantity::from_double(0.0001);
            break;

        case core::Exchange::Kraken:
            config.name = "Kraken";
            config.fees.maker_fee = 0.0016;   // 0.16%
            config.fees.taker_fee = 0.0026;   // 0.26%
            config.latency.order_submit_mean_us = 800;
            config.latency.cancel_mean_us = 600;
            config.latency.market_data_mean_us = 150;
            config.min_order_size = core::Quantity::from_double(0.0001);
            break;

        case core::Exchange::Bybit:
            config.name = "Bybit";
            config.fees.maker_fee = 0.0001;
            config.fees.taker_fee = 0.0006;
            config.latency.order_submit_mean_us = 400;
            config.latency.cancel_mean_us = 300;
            config.latency.market_data_mean_us = 80;
            config.min_order_size = core::Quantity::from_double(0.001);
            break;

        case core::Exchange::OKX:
            config.name = "OKX";
            config.fees.maker_fee = 0.0008;
            config.fees.taker_fee = 0.001;
            config.latency.order_submit_mean_us = 350;
            config.latency.cancel_mean_us = 250;
            config.latency.market_data_mean_us = 70;
            config.min_order_size = core::Quantity::from_double(0.0001);
            break;

        default:
            config.name = "Generic";
            config.fees.maker_fee = 0.001;
            config.fees.taker_fee = 0.001;
            config.latency.order_submit_mean_us = 500;
            config.latency.cancel_mean_us = 400;
            config.latency.market_data_mean_us = 100;
            break;
    }

    return config;
}

}  // namespace hft::backtesting
