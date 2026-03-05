/**
 * @file basic_market_maker.cpp
 * @brief Implementation of Basic Market Making Strategy
 */

#include "strategies/market_making/basic_market_maker.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace hft {
namespace strategies {
namespace market_making {

// ============================================================================
// Constructor
// ============================================================================

BasicMarketMaker::BasicMarketMaker(StrategyConfig config, BasicMMConfig mm_config)
    : StrategyBase(std::move(config))
    , mm_config_(std::move(mm_config))
{
    // Initialize config values from strategy config if not set
    if (mm_config_.max_position <= 0) {
        mm_config_.max_position = config_.max_position_quantity;
    }
    if (mm_config_.max_daily_loss <= 0) {
        mm_config_.max_daily_loss = config_.max_daily_loss;
    }
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

bool BasicMarketMaker::initialize() {
    if (!StrategyBase::initialize()) {
        return false;
    }

    // Clear state
    active_quote_ = ActiveQuote{};
    quote_stats_ = QuoteStats{};
    pending_orders_.clear();
    recent_spreads_.clear();
    last_mid_price_ = 0.0;

    return true;
}

bool BasicMarketMaker::start() {
    if (!StrategyBase::start()) {
        return false;
    }

    last_quote_update_time_ = current_timestamp();
    return true;
}

void BasicMarketMaker::stop() {
    // Cancel all quotes before stopping
    cancel_quotes();
    StrategyBase::stop();
}

void BasicMarketMaker::reset() {
    cancel_quotes();
    active_quote_ = ActiveQuote{};
    quote_stats_ = QuoteStats{};
    pending_orders_.clear();
    recent_spreads_.clear();
    last_mid_price_ = 0.0;
    last_bid_price_ = 0.0;
    last_ask_price_ = 0.0;
    StrategyBase::reset();
}

void BasicMarketMaker::on_stop() {
    cancel_quotes();
}

void BasicMarketMaker::on_reset() {
    active_quote_ = ActiveQuote{};
    quote_stats_ = QuoteStats{};
}

// ============================================================================
// Market Data Handler
// ============================================================================

void BasicMarketMaker::on_market_data(const MarketData& data) {
    if (!is_running()) {
        return;
    }

    // Validate market data
    if (data.bid_price <= 0 || data.ask_price <= 0 || data.ask_price <= data.bid_price) {
        return;
    }

    // Update last market data time
    last_market_data_time_ = data.timestamp;
    Price current_mid = data.mid_price();

    // Check if we need to refresh quotes
    if (should_refresh_quotes(current_mid)) {
        update_quotes(data);
    }

    // Update unrealized PnL
    auto pos = get_position(data.symbol);
    if (pos.has_value() && !pos->is_flat()) {
        update_mark_price(data.symbol, current_mid);
    }

    // Store last mid price
    last_mid_price_ = current_mid;
}

// ============================================================================
// Order Event Handlers
// ============================================================================

void BasicMarketMaker::on_order_update(const OrderUpdate& update) {
    // Track pending orders
    if (update.is_active()) {
        pending_orders_[update.order_id] = update;
    } else if (update.is_terminal()) {
        pending_orders_.erase(update.order_id);
    }

    // Update active quote tracking
    if (update.order_id == active_quote_.bid_order_id) {
        if (update.status == OrderStatus::CANCELLED || update.status == OrderStatus::REJECTED ||
            update.status == OrderStatus::EXPIRED) {
            active_quote_.bid_active = false;
            active_quote_.bid_order_id = 0;
            quote_stats_.quotes_canceled++;
        } else if (update.status == OrderStatus::FILLED) {
            active_quote_.bid_active = false;
            quote_stats_.quotes_filled++;
        } else if (update.status == OrderStatus::PARTIALLY_FILLED) {
            quote_stats_.partial_fills++;
        }
    }

    if (update.order_id == active_quote_.ask_order_id) {
        if (update.status == OrderStatus::CANCELLED || update.status == OrderStatus::REJECTED ||
            update.status == OrderStatus::EXPIRED) {
            active_quote_.ask_active = false;
            active_quote_.ask_order_id = 0;
            quote_stats_.quotes_canceled++;
        } else if (update.status == OrderStatus::FILLED) {
            active_quote_.ask_active = false;
            quote_stats_.quotes_filled++;
        } else if (update.status == OrderStatus::PARTIALLY_FILLED) {
            quote_stats_.partial_fills++;
        }
    }
}

void BasicMarketMaker::on_trade(const Trade& trade) {
    // Update position
    update_position(trade);

    // Update quote stats
    quote_stats_.total_volume_filled += trade.quantity;
    quote_stats_.last_fill_time = trade.timestamp;

    // Update risk metrics
    update_risk_metrics();

    // Check if we need new quotes after fill
    if (!active_quote_.has_active_quote()) {
        // Both sides filled or canceled, need to re-quote
        // This will be handled on next market data update
    }
}

// ============================================================================
// Quote Calculation
// ============================================================================

std::pair<Price, Price> BasicMarketMaker::calculate_quotes(Price mid_price) const {
    // Calculate base spread
    double spread_bps = mm_config_.target_spread_bps;

    // Clamp spread to limits
    spread_bps = std::clamp(spread_bps, mm_config_.min_spread_bps, mm_config_.max_spread_bps);

    // Convert BPS to price
    double half_spread = (spread_bps / 10000.0) * mid_price / 2.0;

    // Calculate inventory skew
    double skew = calculate_inventory_skew();

    // Apply skew: positive skew raises bid and ask (when short, want to buy)
    //            negative skew lowers bid and ask (when long, want to sell)
    double skew_amount = skew * half_spread * mm_config_.inventory_skew_factor;

    // Calculate bid and ask prices
    Price bid_price = mid_price - half_spread + skew_amount;
    Price ask_price = mid_price + half_spread + skew_amount;

    // Ensure minimum spread
    double min_spread = (mm_config_.min_spread_bps / 10000.0) * mid_price;
    if (ask_price - bid_price < min_spread) {
        double adjustment = (min_spread - (ask_price - bid_price)) / 2.0;
        bid_price -= adjustment;
        ask_price += adjustment;
    }

    // Round to tick size
    bid_price = round_price(bid_price);
    ask_price = round_price(ask_price);

    // Ensure bid < ask after rounding
    if (bid_price >= ask_price) {
        ask_price = bid_price + mm_config_.tick_size;
    }

    return {bid_price, ask_price};
}

Quantity BasicMarketMaker::calculate_order_size(OrderSide side) const {
    Quantity base_size = mm_config_.base_order_size;

    // Get current position
    double current_position = 0.0;
    for (const auto& [symbol, pos] : positions_) {
        current_position = pos.quantity;
        break;  // Single symbol strategy
    }

    // Calculate available room for position
    double max_pos = mm_config_.max_position;
    double remaining_long = max_pos - current_position;
    double remaining_short = max_pos + current_position;

    // Adjust size based on inventory
    Quantity adjusted_size = base_size;

    if (side == OrderSide::BUY) {
        // Limit buy size to remaining long capacity
        adjusted_size = std::min(adjusted_size, std::max(0.0, remaining_long));

        // Reduce size when already long (don't want to add more)
        if (current_position > 0) {
            double position_ratio = current_position / max_pos;
            adjusted_size *= (1.0 - position_ratio * mm_config_.inventory_skew_factor);
        }
    } else {
        // Limit sell size to remaining short capacity
        adjusted_size = std::min(adjusted_size, std::max(0.0, remaining_short));

        // Reduce size when already short
        if (current_position < 0) {
            double position_ratio = -current_position / max_pos;
            adjusted_size *= (1.0 - position_ratio * mm_config_.inventory_skew_factor);
        }
    }

    // Apply min/max limits
    adjusted_size = std::clamp(adjusted_size, mm_config_.min_order_size, mm_config_.max_order_size);

    // Round to lot size
    return round_quantity(adjusted_size);
}

// ============================================================================
// Quote Management
// ============================================================================

void BasicMarketMaker::submit_quotes(const MarketData& data) {
    if (!can_quote()) {
        return;
    }

    Price mid_price = data.mid_price();
    auto [bid_price, ask_price] = calculate_quotes(mid_price);
    Quantity bid_size = calculate_order_size(OrderSide::BUY);
    Quantity ask_size = calculate_order_size(OrderSide::SELL);

    // Validate sizes
    if (bid_size < mm_config_.min_order_size && ask_size < mm_config_.min_order_size) {
        return;  // Can't quote
    }

    // Generate signals for bid and ask
    if (bid_size >= mm_config_.min_order_size) {
        generate_order_signal(OrderSide::BUY, bid_price, bid_size);
        active_quote_.bid_price = bid_price;
        active_quote_.bid_qty = bid_size;
        active_quote_.bid_active = true;
    }

    if (ask_size >= mm_config_.min_order_size) {
        generate_order_signal(OrderSide::SELL, ask_price, ask_size);
        active_quote_.ask_price = ask_price;
        active_quote_.ask_qty = ask_size;
        active_quote_.ask_active = true;
    }

    // Update stats
    active_quote_.quote_time = data.timestamp;
    quote_stats_.quotes_sent++;
    quote_stats_.total_volume_quoted += bid_size + ask_size;
    quote_stats_.last_quote_time = data.timestamp;

    // Track spread
    double spread_bps = ((ask_price - bid_price) / mid_price) * 10000.0;
    recent_spreads_.push_back(spread_bps);
    if (recent_spreads_.size() > MAX_SPREAD_HISTORY) {
        recent_spreads_.pop_front();
    }

    update_quote_stats();

    last_quote_update_time_ = data.timestamp;
    last_bid_price_ = bid_price;
    last_ask_price_ = ask_price;
}

void BasicMarketMaker::cancel_quotes() {
    if (active_quote_.bid_active && active_quote_.bid_order_id > 0) {
        // Generate cancel signal for bid
        Signal cancel_signal;
        cancel_signal.type = SignalType::CLOSE_LONG;  // Cancel buy
        cancel_signal.symbol = config_.symbols.empty() ? "" : config_.symbols[0];
        cancel_signal.target_quantity = 0;
        emit_signal(std::move(cancel_signal));
        active_quote_.bid_active = false;
    }

    if (active_quote_.ask_active && active_quote_.ask_order_id > 0) {
        // Generate cancel signal for ask
        Signal cancel_signal;
        cancel_signal.type = SignalType::CLOSE_SHORT;  // Cancel sell
        cancel_signal.symbol = config_.symbols.empty() ? "" : config_.symbols[0];
        cancel_signal.target_quantity = 0;
        emit_signal(std::move(cancel_signal));
        active_quote_.ask_active = false;
    }
}

void BasicMarketMaker::update_quotes(const MarketData& data) {
    // Cancel existing quotes
    cancel_quotes();

    // Submit new quotes
    submit_quotes(data);
}

bool BasicMarketMaker::should_refresh_quotes(Price current_mid) const {
    // Check if we have active quotes
    if (!active_quote_.has_active_quote()) {
        return true;  // No quotes, need to place
    }

    // Check quote timeout
    auto now = current_timestamp();
    auto quote_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - active_quote_.quote_time).count();

    if (quote_age_ms > mm_config_.quote_refresh_interval_ms) {
        return true;  // Quote too old
    }

    // Check price deviation
    if (last_mid_price_ > 0 && current_mid > 0) {
        double price_move = std::abs(current_mid - last_mid_price_) / last_mid_price_;
        if (price_move > PRICE_DEVIATION_THRESHOLD) {
            return true;  // Price moved significantly
        }
    }

    // Check if quote is still competitive
    if (active_quote_.bid_active && current_mid - active_quote_.bid_price >
        (mm_config_.max_spread_bps / 10000.0) * current_mid / 2.0) {
        return true;  // Bid too far from mid
    }

    if (active_quote_.ask_active && active_quote_.ask_price - current_mid >
        (mm_config_.max_spread_bps / 10000.0) * current_mid / 2.0) {
        return true;  // Ask too far from mid
    }

    return false;
}

// ============================================================================
// Helper Methods
// ============================================================================

double BasicMarketMaker::calculate_inventory_skew() const {
    // Get current position
    double current_position = 0.0;
    for (const auto& [symbol, pos] : positions_) {
        current_position = pos.quantity;
        break;
    }

    // Calculate position ratio (-1 to 1)
    double max_pos = mm_config_.max_position;
    if (max_pos <= 0) return 0.0;

    double position_ratio = current_position / max_pos;
    position_ratio = std::clamp(position_ratio, -1.0, 1.0);

    // Calculate deviation from target
    double target = mm_config_.target_position;
    double deviation = (current_position - target) / max_pos;
    deviation = std::clamp(deviation, -1.0, 1.0);

    // Return skew: positive when short (want to buy), negative when long (want to sell)
    return -deviation;
}

bool BasicMarketMaker::can_quote() const {
    // Check if running
    if (!is_running()) {
        return false;
    }

    // Check risk limits
    if (!check_risk_limits()) {
        return false;
    }

    // Check max open orders
    if (static_cast<int>(pending_orders_.size()) >= mm_config_.max_open_orders) {
        return false;
    }

    // Check daily loss limit
    if (risk_metrics_.daily_pnl < -mm_config_.max_daily_loss) {
        return false;
    }

    return true;
}

Price BasicMarketMaker::round_price(Price price) const {
    if (mm_config_.tick_size <= 0) return price;
    return std::round(price / mm_config_.tick_size) * mm_config_.tick_size;
}

Quantity BasicMarketMaker::round_quantity(Quantity qty) const {
    if (mm_config_.lot_size <= 0) return qty;
    return std::round(qty / mm_config_.lot_size) * mm_config_.lot_size;
}

void BasicMarketMaker::generate_order_signal(OrderSide side, Price price, Quantity quantity) {
    Signal signal;
    signal.type = (side == OrderSide::BUY) ? SignalType::BUY : SignalType::SELL;
    signal.symbol = config_.symbols.empty() ? "" : config_.symbols[0];
    signal.target_price = price;
    signal.target_quantity = quantity;
    signal.confidence = 1.0;
    signal.urgency = 0.5;  // Normal urgency for market making

    emit_signal(std::move(signal));
}

void BasicMarketMaker::update_quote_stats() {
    // Calculate average spread
    if (!recent_spreads_.empty()) {
        double sum = 0.0;
        for (double spread : recent_spreads_) {
            sum += spread;
        }
        quote_stats_.average_spread_bps = sum / recent_spreads_.size();
    }

    // Calculate fill rate
    if (quote_stats_.quotes_sent > 0) {
        quote_stats_.fill_rate = static_cast<double>(quote_stats_.quotes_filled) /
                                 static_cast<double>(quote_stats_.quotes_sent);
    }
}

void BasicMarketMaker::set_mm_config(const BasicMMConfig& config) {
    mm_config_ = config;
}

}  // namespace market_making
}  // namespace strategies
}  // namespace hft
