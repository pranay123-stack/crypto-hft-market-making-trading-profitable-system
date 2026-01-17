#include "strategy/market_maker.hpp"
#include <algorithm>
#include <cmath>

namespace hft {

// ============================================================================
// MarketMaker Implementation
// ============================================================================

MarketMaker::MarketMaker(const MarketMakerParams& params)
    : params_(params)
{
}

QuoteDecision MarketMaker::compute_quotes(
    const OrderBook& book,
    Quantity current_position,
    const Signal& signal)
{
    QuoteDecision decision;

    if (!enabled_) {
        decision.reason = "Strategy disabled";
        return decision;
    }

    if (!book.is_valid()) {
        decision.reason = "Invalid orderbook";
        return decision;
    }

    // Calculate fair value
    Price fair_value = calculate_fair_value(book);
    if (fair_value == 0) {
        decision.reason = "Cannot determine fair value";
        return decision;
    }

    // Calculate spread
    double spread_bps = calculate_spread(book, signal);
    Price half_spread = static_cast<Price>(
        fair_value * spread_bps / 20000.0);  // bps to half-spread

    // Calculate inventory skew
    double skew = calculate_inventory_skew(current_position);

    // Apply skew to prices
    Price skew_adjustment = static_cast<Price>(fair_value * skew * params_.inventory_skew / 10000.0);

    decision.bid_price = fair_value - half_spread - skew_adjustment;
    decision.ask_price = fair_value + half_spread - skew_adjustment;

    // Ensure prices don't cross
    if (decision.bid_price >= decision.ask_price) {
        decision.reason = "Prices would cross";
        return decision;
    }

    // Calculate order sizes
    decision.bid_size = calculate_order_size(Side::BUY, current_position);
    decision.ask_size = calculate_order_size(Side::SELL, current_position);

    // Don't quote if sizes are zero
    if (decision.bid_size == 0 && decision.ask_size == 0) {
        decision.reason = "Order sizes are zero";
        return decision;
    }

    // Check if we should skip quoting to avoid excessive updates
    Timestamp now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (now - last_quote_time_ < params_.min_quote_life_us * 1000) {
        // Check if prices changed significantly
        Price bid_diff = std::abs(decision.bid_price - active_bid_price_);
        Price ask_diff = std::abs(decision.ask_price - active_ask_price_);
        Price threshold = fair_value / 10000;  // 1 bps

        if (bid_diff < threshold && ask_diff < threshold) {
            decision.reason = "Prices unchanged";
            return decision;
        }
    }

    decision.should_quote = true;
    last_quote_time_ = now;

    return decision;
}

void MarketMaker::on_trade(const Trade& trade) {
    // Track market trades for signal generation
    // Subclasses can override for specific behavior
}

void MarketMaker::on_fill(const Order& order, Quantity filled_qty, Price fill_price) {
    fills_++;

    if (order.side == Side::BUY) {
        total_bought_ += filled_qty;
    } else {
        total_sold_ += filled_qty;
    }

    // Update realized PnL (simplified)
    // In production, would track cost basis properly
}

void MarketMaker::on_cancel(OrderId order_id) {
    if (order_id == active_bid_id_) {
        active_bid_id_ = 0;
        active_bid_price_ = 0;
    } else if (order_id == active_ask_id_) {
        active_ask_id_ = 0;
        active_ask_price_ = 0;
    }
}

void MarketMaker::on_reject(OrderId order_id, const std::string& reason) {
    on_cancel(order_id);  // Same handling as cancel
}

void MarketMaker::update_params(const MarketMakerParams& params) {
    params_ = params;
}

Price MarketMaker::calculate_fair_value(const OrderBook& book) {
    // Simple mid-price based fair value
    // Subclasses can implement more sophisticated calculations
    return book.mid_price();
}

double MarketMaker::calculate_spread(const OrderBook& book, const Signal& signal) {
    // Base spread from params
    double spread = params_.target_spread_bps;

    // Adjust for volatility
    if (signal.volatility > 0) {
        spread *= (1.0 + signal.volatility);
    }

    // Clamp to min/max
    spread = std::clamp(spread, params_.min_spread_bps, params_.max_spread_bps);

    return spread;
}

Quantity MarketMaker::calculate_order_size(Side side, Quantity position) {
    Quantity base_size = params_.default_order_size;

    // Reduce size on side that would increase position
    if (params_.max_position > 0) {
        if (side == Side::BUY && position > 0) {
            // Already long, reduce buy size
            double ratio = 1.0 - static_cast<double>(position) /
                          static_cast<double>(params_.max_position);
            base_size = static_cast<Quantity>(base_size * std::max(0.0, ratio));
        } else if (side == Side::SELL && position < 0) {
            // Already short, reduce sell size
            double ratio = 1.0 + static_cast<double>(position) /
                          static_cast<double>(params_.max_position);
            base_size = static_cast<Quantity>(base_size * std::max(0.0, ratio));
        }
    }

    // Clamp to min/max
    base_size = std::clamp(base_size, params_.min_order_size, params_.max_order_size);

    return base_size;
}

double MarketMaker::calculate_inventory_skew(Quantity position) {
    if (params_.max_position == 0) return 0.0;

    // Linear skew based on position as fraction of max
    return static_cast<double>(position) / static_cast<double>(params_.max_position);
}

OrderId MarketMaker::send_order(const Order& order) {
    if (!order_callback_) return 0;

    OrderId id = order_callback_(order);

    if (id != 0) {
        quotes_sent_++;
        if (order.side == Side::BUY) {
            active_bid_id_ = id;
            active_bid_price_ = order.price;
        } else {
            active_ask_id_ = id;
            active_ask_price_ = order.price;
        }
    }

    return id;
}

bool MarketMaker::cancel_order(OrderId order_id) {
    if (!cancel_callback_) return false;
    return cancel_callback_(order_id);
}

// ============================================================================
// InventoryAdjustedMM Implementation
// ============================================================================

InventoryAdjustedMM::InventoryAdjustedMM(const MarketMakerParams& params)
    : MarketMaker(params)
{
}

QuoteDecision InventoryAdjustedMM::compute_quotes(
    const OrderBook& book,
    Quantity current_position,
    const Signal& signal)
{
    // Update EMA of position
    ema_position_ = ema_alpha_ * current_position + (1.0 - ema_alpha_) * ema_position_;

    return MarketMaker::compute_quotes(book, current_position, signal);
}

double InventoryAdjustedMM::calculate_inventory_skew(Quantity position) {
    if (params_.max_position == 0) return 0.0;

    // Use EMA position for smoother skew
    double normalized = ema_position_ / static_cast<double>(params_.max_position);

    // Apply sigmoid for non-linear skew at extremes
    double sigmoid = 2.0 / (1.0 + std::exp(-3.0 * normalized)) - 1.0;

    return sigmoid;
}

// ============================================================================
// AvellanedaStoikovMM Implementation
// ============================================================================

AvellanedaStoikovMM::AvellanedaStoikovMM(
    const MarketMakerParams& base_params,
    const ASParams& as_params)
    : MarketMaker(base_params)
    , as_params_(as_params)
{
}

QuoteDecision AvellanedaStoikovMM::compute_quotes(
    const OrderBook& book,
    Quantity current_position,
    const Signal& signal)
{
    QuoteDecision decision;

    if (!enabled_ || !book.is_valid()) {
        decision.reason = "Disabled or invalid book";
        return decision;
    }

    // Initialize start time
    if (start_time_ == 0) {
        start_time_ = signal.timestamp;
    }

    // Calculate time remaining (normalized to [0, 1])
    Timestamp elapsed = signal.timestamp - start_time_;
    double t_elapsed = static_cast<double>(elapsed) / 1e9 / as_params_.T;
    double t_remaining = std::max(0.01, 1.0 - std::fmod(t_elapsed, 1.0));

    Price mid = book.mid_price();

    // Calculate reservation price (indifference price)
    Price reservation = calculate_reservation_price(mid, current_position, t_remaining);

    // Calculate optimal spread
    double spread = calculate_optimal_spread(t_remaining);
    Price half_spread = static_cast<Price>(mid * spread / 20000.0);

    decision.bid_price = reservation - half_spread;
    decision.ask_price = reservation + half_spread;

    // Ensure no crossing
    if (decision.bid_price >= decision.ask_price) {
        decision.reason = "Prices would cross";
        return decision;
    }

    decision.bid_size = calculate_order_size(Side::BUY, current_position);
    decision.ask_size = calculate_order_size(Side::SELL, current_position);

    if (decision.bid_size > 0 || decision.ask_size > 0) {
        decision.should_quote = true;
    }

    return decision;
}

Price AvellanedaStoikovMM::calculate_reservation_price(
    Price mid,
    Quantity position,
    double t_remaining)
{
    // r(s,q,t) = s - q * gamma * sigma^2 * (T - t)
    double adjustment = static_cast<double>(position) *
                       as_params_.gamma *
                       as_params_.sigma * as_params_.sigma *
                       t_remaining;

    return mid - static_cast<Price>(mid * adjustment);
}

double AvellanedaStoikovMM::calculate_optimal_spread(double t_remaining) {
    // delta = gamma * sigma^2 * (T - t) + (2/gamma) * ln(1 + gamma/k)
    double term1 = as_params_.gamma * as_params_.sigma * as_params_.sigma * t_remaining;
    double term2 = (2.0 / as_params_.gamma) *
                   std::log(1.0 + as_params_.gamma / as_params_.k);

    double spread_bps = (term1 + term2) * 10000.0;

    // Clamp to configured limits
    return std::clamp(spread_bps, params_.min_spread_bps, params_.max_spread_bps);
}

} // namespace hft
