/**
 * @file inventory_market_maker.cpp
 * @brief Implementation of Inventory-Aware Market Making Strategy
 */

#include "strategies/market_making/inventory_market_maker.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace hft {
namespace strategies {
namespace market_making {

// ============================================================================
// Constructor
// ============================================================================

InventoryMarketMaker::InventoryMarketMaker(StrategyConfig config, InventoryMMConfig mm_config)
    : BasicMarketMaker(std::move(config), mm_config)
    , inventory_config_(std::move(mm_config))
{
    mm_config_ = inventory_config_;
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

bool InventoryMarketMaker::initialize() {
    if (!BasicMarketMaker::initialize()) {
        return false;
    }

    // Initialize state
    inventory_analysis_ = InventoryAnalysis{};
    adverse_selection_metrics_ = AdverseSelectionMetrics{};
    pnl_tracker_ = PnLTracker{};

    // Clear history
    fill_events_.clear();
    pending_fill_checks_.clear();
    pnl_history_.clear();
    inventory_history_.clear();

    average_entry_price_ = 0.0;
    position_cost_basis_ = 0.0;

    return true;
}

void InventoryMarketMaker::reset() {
    BasicMarketMaker::reset();
    on_reset();
}

void InventoryMarketMaker::on_reset() {
    inventory_analysis_ = InventoryAnalysis{};
    adverse_selection_metrics_ = AdverseSelectionMetrics{};
    pnl_tracker_ = PnLTracker{};
    fill_events_.clear();
    pending_fill_checks_.clear();
    pnl_history_.clear();
    inventory_history_.clear();
    average_entry_price_ = 0.0;
    position_cost_basis_ = 0.0;
}

// ============================================================================
// Market Data Handler
// ============================================================================

void InventoryMarketMaker::on_market_data(const MarketData& data) {
    if (!is_running()) {
        return;
    }

    Price mid_price = data.mid_price();

    // Update PnL tracker
    update_pnl_tracker(mid_price);

    // Update adverse selection metrics
    update_adverse_selection_metrics(data);

    // Check pending fill price movements
    auto now = data.timestamp;
    while (!pending_fill_checks_.empty() &&
           pending_fill_checks_.front().check_time <= now) {
        auto& check = pending_fill_checks_.front();

        // Find the fill event and update price movement
        for (auto& fe : fill_events_) {
            if (fe.order_id == check.order_id) {
                fe.mid_price_after = mid_price;
                double move = (mid_price - check.mid_at_fill) / check.mid_at_fill;
                fe.price_move_bps = move * 10000.0;

                // Adverse if price moved against us
                if (check.side == OrderSide::BUY) {
                    fe.was_adverse = (mid_price < check.mid_at_fill);
                } else {
                    fe.was_adverse = (mid_price > check.mid_at_fill);
                }

                // Update adverse selection tracking
                if (fe.was_adverse) {
                    adverse_selection_metrics_.consecutive_adverse_fills++;
                    if (check.side == OrderSide::BUY) {
                        adverse_selection_metrics_.post_fill_move_bid += fe.price_move_bps;
                    } else {
                        adverse_selection_metrics_.post_fill_move_ask += fe.price_move_bps;
                    }
                } else {
                    adverse_selection_metrics_.consecutive_adverse_fills = 0;
                }
                break;
            }
        }
        pending_fill_checks_.pop_front();
    }

    // Update inventory analysis
    inventory_analysis_ = analyze_inventory();

    // Check for emergency conditions
    if (should_liquidate()) {
        initiate_liquidation();
        return;
    }

    // Check stop loss
    if (check_stop_loss()) {
        initiate_liquidation();
        return;
    }

    // Call base class (uses our overridden quote calculation)
    BasicMarketMaker::on_market_data(data);

    // Track inventory history
    InventorySample inv_sample{inventory_analysis_.current_inventory, data.timestamp};
    inventory_history_.push_back(inv_sample);
    while (inventory_history_.size() > 1000) {
        inventory_history_.pop_front();
    }
}

// ============================================================================
// Order Event Handlers
// ============================================================================

void InventoryMarketMaker::on_order_update(const OrderUpdate& update) {
    BasicMarketMaker::on_order_update(update);
}

void InventoryMarketMaker::on_trade(const Trade& trade) {
    // Record fill for adverse selection tracking
    record_fill_event(trade, last_mid_price_);

    // Update entry price tracking
    double old_qty = 0.0;
    for (const auto& [sym, pos] : positions_) {
        old_qty = pos.quantity;
        break;
    }

    // Call base class to update position
    BasicMarketMaker::on_trade(trade);

    // Get new position
    double new_qty = 0.0;
    for (const auto& [sym, pos] : positions_) {
        new_qty = pos.quantity;
        average_entry_price_ = pos.average_entry_price;
        break;
    }

    // Update cost basis
    double trade_qty = (trade.side == OrderSide::BUY) ? trade.quantity : -trade.quantity;
    if ((old_qty >= 0 && trade_qty > 0) || (old_qty <= 0 && trade_qty < 0)) {
        // Adding to position
        position_cost_basis_ += trade.price * std::abs(trade.quantity);
    } else {
        // Reducing position
        double reduced = std::min(std::abs(old_qty), std::abs(trade_qty));
        if (std::abs(old_qty) > 0) {
            position_cost_basis_ *= (1.0 - reduced / std::abs(old_qty));
        }
    }

    // Update PnL tracker with realized PnL
    double realized = trade.notional() - (average_entry_price_ * trade.quantity);
    if (trade.side == OrderSide::SELL && old_qty > 0) {
        realized = (trade.price - average_entry_price_) * std::min(old_qty, trade.quantity);
    } else if (trade.side == OrderSide::BUY && old_qty < 0) {
        realized = (average_entry_price_ - trade.price) * std::min(-old_qty, trade.quantity);
    } else {
        realized = 0;  // Adding to position, no realized PnL
    }

    pnl_tracker_.realized_pnl += realized;
    if (realized > 0) {
        pnl_tracker_.winning_trades++;
    } else if (realized < 0) {
        pnl_tracker_.losing_trades++;
    }
}

// ============================================================================
// Inventory Management
// ============================================================================

InventoryAnalysis InventoryMarketMaker::analyze_inventory() const {
    InventoryAnalysis analysis;

    // Get current position
    for (const auto& [sym, pos] : positions_) {
        analysis.current_inventory = pos.quantity;
        analysis.inventory_value = pos.notional();
        analysis.inventory_pnl = pos.unrealized_pnl;
        break;
    }

    // Calculate inventory ratio
    double max_inv = inventory_config_.max_position;
    if (max_inv > 0) {
        analysis.inventory_ratio = analysis.current_inventory / max_inv;
        analysis.inventory_ratio = std::clamp(analysis.inventory_ratio, -1.0, 1.0);
    }

    // Calculate deviation from target
    analysis.target_deviation = std::abs(analysis.current_inventory -
                                         inventory_config_.target_inventory) / max_inv;

    // Determine mode
    analysis.current_mode = determine_inventory_mode();

    // Calculate required skew
    analysis.required_skew_bps = calculate_inventory_skew();

    // Estimate time to target
    analysis.time_to_target_seconds = estimate_time_to_target();

    return analysis;
}

double InventoryMarketMaker::calculate_inventory_skew() const {
    double current_inv = inventory_analysis_.current_inventory;
    double target_inv = inventory_config_.target_inventory;
    double max_inv = inventory_config_.max_position;

    if (max_inv <= 0) return 0.0;

    // Calculate deviation from target
    double deviation = current_inv - target_inv;
    double deviation_ratio = deviation / max_inv;

    // Linear skew component
    double linear_skew = -deviation_ratio * inventory_config_.skew_per_unit_bps * max_inv;

    // Quadratic skew for aggressive inventory management
    double quadratic_skew = 0.0;
    if (inventory_config_.enable_aggressive_skew) {
        quadratic_skew = -std::copysign(1.0, deviation) *
                         deviation_ratio * deviation_ratio *
                         inventory_config_.skew_per_unit_bps * max_inv;
    }

    double total_skew = linear_skew + quadratic_skew;

    // Clamp to max skew
    total_skew = std::clamp(total_skew, -inventory_config_.max_skew_bps,
                           inventory_config_.max_skew_bps);

    return total_skew;
}

InventoryMode InventoryMarketMaker::determine_inventory_mode() const {
    double deviation = inventory_analysis_.target_deviation;

    if (deviation >= inventory_config_.critical_inventory_deviation) {
        return InventoryMode::LIQUIDATE;
    } else if (deviation >= inventory_config_.max_inventory_deviation) {
        return InventoryMode::AGGRESSIVE;
    } else if (deviation >= inventory_config_.mean_reversion_threshold) {
        return InventoryMode::NEUTRAL;
    } else {
        return InventoryMode::PASSIVE;
    }
}

double InventoryMarketMaker::calculate_target_inventory() const {
    // For now, just return the configured target
    // Could be extended to incorporate market signals
    return inventory_config_.target_inventory;
}

double InventoryMarketMaker::estimate_time_to_target() const {
    if (inventory_history_.size() < 10) {
        return inventory_config_.inventory_half_life_seconds;
    }

    // Estimate based on recent inventory changes
    double inventory_velocity = 0.0;
    for (size_t i = 1; i < inventory_history_.size(); ++i) {
        auto dt = (inventory_history_[i].time - inventory_history_[i-1].time).count() / 1e9;
        if (dt > 0) {
            double dq = inventory_history_[i].inventory - inventory_history_[i-1].inventory;
            inventory_velocity += dq / dt;
        }
    }
    inventory_velocity /= (inventory_history_.size() - 1);

    double distance = std::abs(inventory_analysis_.current_inventory -
                               inventory_config_.target_inventory);

    if (std::abs(inventory_velocity) > 1e-10) {
        return distance / std::abs(inventory_velocity);
    }

    return inventory_config_.inventory_half_life_seconds;
}

// ============================================================================
// Adverse Selection Protection
// ============================================================================

void InventoryMarketMaker::update_adverse_selection_metrics(const MarketData& data) {
    // Calculate fill rates per side
    int bid_quotes = 0, bid_fills = 0;
    int ask_quotes = 0, ask_fills = 0;

    int window = inventory_config_.adverse_selection_window;
    int count = 0;

    for (auto it = fill_events_.rbegin(); it != fill_events_.rend() && count < window; ++it, ++count) {
        if (it->side == OrderSide::BUY) {
            bid_quotes++;
            bid_fills++;  // These are only fills
        } else {
            ask_quotes++;
            ask_fills++;
        }
    }

    if (bid_quotes > 0) {
        adverse_selection_metrics_.fill_rate_bid =
            static_cast<double>(bid_fills) / bid_quotes;
    }
    if (ask_quotes > 0) {
        adverse_selection_metrics_.fill_rate_ask =
            static_cast<double>(ask_fills) / ask_quotes;
    }

    // Calculate adverse selection score
    double adverse_moves = 0.0;
    count = 0;
    for (auto it = fill_events_.rbegin(); it != fill_events_.rend() && count < window; ++it, ++count) {
        if (it->was_adverse) {
            adverse_moves++;
        }
    }

    if (count > 0) {
        adverse_selection_metrics_.adverse_selection_score = adverse_moves / count;
    }

    // Detect adverse selection
    adverse_selection_metrics_.adverse_selection_detected =
        adverse_selection_metrics_.adverse_selection_score >
        inventory_config_.adverse_selection_threshold;
}

bool InventoryMarketMaker::is_adverse_selection_detected() const {
    return adverse_selection_metrics_.adverse_selection_detected;
}

double InventoryMarketMaker::calculate_adverse_selection_adjustment() const {
    if (!adverse_selection_metrics_.adverse_selection_detected) {
        return 1.0;
    }

    // Widen spread when adverse selection detected
    double adjustment = inventory_config_.adverse_selection_spread_mult;

    // Further increase based on consecutive adverse fills
    int consecutive = adverse_selection_metrics_.consecutive_adverse_fills;
    adjustment += 0.1 * std::min(consecutive, 5);

    return adjustment;
}

void InventoryMarketMaker::record_fill_event(const Trade& fill, Price mid_price) {
    FillEvent event;
    event.order_id = fill.order_id;
    event.side = fill.side;
    event.fill_price = fill.price;
    event.fill_qty = fill.quantity;
    event.mid_price_at_fill = mid_price;
    event.fill_time = fill.timestamp;

    fill_events_.push_back(event);

    // Trim old events
    while (fill_events_.size() > MAX_FILL_EVENTS) {
        fill_events_.pop_front();
    }

    // Schedule price check
    PendingFillCheck check;
    check.order_id = fill.order_id;
    check.fill_price = fill.price;
    check.mid_at_fill = mid_price;
    check.side = fill.side;
    check.fill_time = fill.timestamp;
    check.check_time = Timestamp{fill.timestamp.count() +
                                  ADVERSE_CHECK_DELAY_MS * 1000000};
    pending_fill_checks_.push_back(check);
}

// ============================================================================
// PnL-Based Adjustment
// ============================================================================

void InventoryMarketMaker::update_pnl_tracker(Price current_price) {
    // Calculate unrealized PnL
    double inventory = inventory_analysis_.current_inventory;
    pnl_tracker_.unrealized_pnl = inventory * (current_price - average_entry_price_);

    // Total PnL
    pnl_tracker_.total_pnl = pnl_tracker_.realized_pnl + pnl_tracker_.unrealized_pnl;

    // Rolling PnL
    PnLSample sample{pnl_tracker_.total_pnl, current_timestamp()};
    pnl_history_.push_back(sample);

    // Trim old samples
    while (!pnl_history_.empty() &&
           (pnl_history_.back().time - pnl_history_.front().time).count() >
           static_cast<int64_t>(inventory_config_.pnl_lookback_seconds * 1e9)) {
        pnl_history_.pop_front();
    }

    if (!pnl_history_.empty()) {
        pnl_tracker_.rolling_pnl = pnl_tracker_.total_pnl - pnl_history_.front().pnl;
    }

    // Update peak and drawdown
    pnl_tracker_.peak_pnl = std::max(pnl_tracker_.peak_pnl, pnl_tracker_.total_pnl);
    pnl_tracker_.drawdown = pnl_tracker_.peak_pnl - pnl_tracker_.total_pnl;
    pnl_tracker_.max_drawdown = std::max(pnl_tracker_.max_drawdown, pnl_tracker_.drawdown);

    // Win rate and profit factor
    int total_trades = pnl_tracker_.winning_trades + pnl_tracker_.losing_trades;
    if (total_trades > 0) {
        pnl_tracker_.win_rate = static_cast<double>(pnl_tracker_.winning_trades) / total_trades;
    }

    pnl_tracker_.last_update = current_timestamp();
}

double InventoryMarketMaker::calculate_pnl_adjustment() const {
    if (!inventory_config_.enable_pnl_adjustment) {
        return 1.0;
    }

    double rolling = pnl_tracker_.rolling_pnl;

    if (rolling < -inventory_config_.pnl_threshold) {
        // Losing money - widen spread
        return inventory_config_.loss_spread_multiplier;
    } else if (rolling > inventory_config_.pnl_threshold) {
        // Making money - can tighten spread
        return inventory_config_.profit_spread_multiplier;
    }

    return 1.0;
}

bool InventoryMarketMaker::check_stop_loss() const {
    if (!inventory_config_.enable_stop_loss) {
        return false;
    }

    // Check position-based stop loss
    double position_value = inventory_analysis_.inventory_value;
    double position_pnl = inventory_analysis_.inventory_pnl;

    if (position_value > 0) {
        double loss_pct = -position_pnl / position_value;
        if (loss_pct > inventory_config_.stop_loss_pct) {
            return true;
        }
    }

    return false;
}

// ============================================================================
// Quote Calculation Override
// ============================================================================

std::pair<Price, Price> InventoryMarketMaker::calculate_quotes(Price mid_price) const {
    // Start with base spread
    double spread_bps = inventory_config_.target_spread_bps;

    // Apply inventory skew
    double skew_bps = calculate_inventory_skew();

    // Apply adverse selection adjustment
    double adverse_mult = calculate_adverse_selection_adjustment();
    spread_bps *= adverse_mult;

    // Apply PnL adjustment
    double pnl_mult = calculate_pnl_adjustment();
    spread_bps *= pnl_mult;

    // Clamp spread
    spread_bps = std::clamp(spread_bps, inventory_config_.min_spread_bps,
                           inventory_config_.max_spread_bps);

    // Calculate half spread
    double half_spread = (spread_bps / 10000.0) * mid_price / 2.0;

    // Calculate prices with skew
    // Skew is in bps, positive = want to buy (raise prices)
    double skew_price = (skew_bps / 10000.0) * mid_price;

    Price bid_price = mid_price - half_spread + skew_price;
    Price ask_price = mid_price + half_spread + skew_price;

    // Apply mean reversion
    auto [mr_bid, mr_ask] = apply_mean_reversion(bid_price, ask_price);

    // Round
    mr_bid = round_price(mr_bid);
    mr_ask = round_price(mr_ask);

    // Ensure proper spread
    if (mr_bid >= mr_ask) {
        mr_ask = mr_bid + mm_config_.tick_size;
    }

    return {mr_bid, mr_ask};
}

Quantity InventoryMarketMaker::calculate_order_size(OrderSide side) const {
    Quantity base_size = inventory_config_.base_order_size;

    // Get current inventory
    double inv = inventory_analysis_.current_inventory;
    double max_inv = inventory_config_.max_position;
    double inv_ratio = max_inv > 0 ? inv / max_inv : 0.0;

    // Asymmetric sizing based on inventory
    if (side == OrderSide::BUY) {
        // Reduce buy size when long
        if (inv > 0) {
            base_size *= (1.0 - inv_ratio * inventory_config_.asymmetric_size_ratio);
        }
        // Check remaining capacity
        double remaining = max_inv - inv;
        base_size = std::min(base_size, std::max(0.0, remaining));
    } else {
        // Reduce sell size when short
        if (inv < 0) {
            base_size *= (1.0 + inv_ratio * inventory_config_.asymmetric_size_ratio);
        }
        // Check remaining capacity
        double remaining = max_inv + inv;
        base_size = std::min(base_size, std::max(0.0, remaining));
    }

    // In liquidation mode, increase size in reducing direction
    if (inventory_analysis_.current_mode == InventoryMode::LIQUIDATE ||
        inventory_analysis_.current_mode == InventoryMode::AGGRESSIVE) {
        if ((side == OrderSide::SELL && inv > 0) ||
            (side == OrderSide::BUY && inv < 0)) {
            base_size *= 1.5;  // 50% larger when reducing inventory
        }
    }

    // Apply limits
    base_size = std::clamp(base_size, inventory_config_.min_order_size,
                          inventory_config_.max_order_size);

    return round_quantity(base_size);
}

// ============================================================================
// Mean Reversion
// ============================================================================

double InventoryMarketMaker::calculate_mean_reversion_signal() const {
    double inv = inventory_analysis_.current_inventory;
    double target = inventory_config_.target_inventory;
    double max_inv = inventory_config_.max_position;

    if (max_inv <= 0) return 0.0;

    // Mean reversion signal: positive = buy, negative = sell
    double deviation = (target - inv) / max_inv;

    // Only apply above threshold
    if (std::abs(deviation) < inventory_config_.mean_reversion_threshold) {
        return 0.0;
    }

    return deviation * inventory_config_.mean_reversion_strength;
}

std::pair<Price, Price> InventoryMarketMaker::apply_mean_reversion(
    Price bid_price, Price ask_price) const {

    double mr_signal = calculate_mean_reversion_signal();

    if (std::abs(mr_signal) < 1e-10) {
        return {bid_price, ask_price};
    }

    // mr_signal > 0 means we want to buy more (inventory below target)
    // So we should raise our bid and potentially raise our ask
    double adjustment = mr_signal * (ask_price - bid_price);

    Price new_bid = bid_price + adjustment * 0.5;
    Price new_ask = ask_price + adjustment * 0.5;

    return {new_bid, new_ask};
}

// ============================================================================
// Emergency Controls
// ============================================================================

bool InventoryMarketMaker::should_liquidate() const {
    double inv_ratio = std::abs(inventory_analysis_.inventory_ratio);

    return inv_ratio >= inventory_config_.emergency_liquidation_threshold ||
           inventory_analysis_.current_mode == InventoryMode::LIQUIDATE;
}

void InventoryMarketMaker::initiate_liquidation() {
    double inv = inventory_analysis_.current_inventory;

    if (std::abs(inv) < inventory_config_.min_order_size) {
        return;  // Nothing to liquidate
    }

    // Generate liquidation signal
    Signal signal;
    signal.type = (inv > 0) ? SignalType::SELL : SignalType::BUY;
    signal.symbol = config_.symbols.empty() ? "" : config_.symbols[0];
    signal.target_quantity = std::abs(inv);
    signal.confidence = 1.0;
    signal.urgency = 1.0;  // Maximum urgency

    // Apply liquidation discount
    if (last_mid_price_ > 0) {
        double discount = (inventory_config_.liquidation_discount_bps / 10000.0) * last_mid_price_;
        if (inv > 0) {
            signal.target_price = last_mid_price_ - discount;  // Sell below mid
        } else {
            signal.target_price = last_mid_price_ + discount;  // Buy above mid
        }
    }

    emit_signal(std::move(signal));
}

}  // namespace market_making
}  // namespace strategies
}  // namespace hft
