/**
 * @file grid_market_maker.cpp
 * @brief Implementation of Grid-Based Market Making Strategy
 */

#include "strategies/market_making/grid_market_maker.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace hft {
namespace strategies {
namespace market_making {

// ============================================================================
// Constructor
// ============================================================================

GridMarketMaker::GridMarketMaker(StrategyConfig config, GridMMConfig mm_config)
    : BasicMarketMaker(std::move(config), mm_config)
    , grid_config_(std::move(mm_config))
{
    mm_config_ = grid_config_;
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

bool GridMarketMaker::initialize() {
    if (!BasicMarketMaker::initialize()) {
        return false;
    }

    // Initialize state
    grid_levels_.clear();
    grid_state_ = GridState{};
    grid_stats_ = GridStats{};
    order_to_level_.clear();
    price_history_.clear();
    current_volatility_ = 0.0;
    consecutive_losses_ = 0;
    current_martingale_mult_ = 1.0;

    return true;
}

bool GridMarketMaker::start() {
    if (!BasicMarketMaker::start()) {
        return false;
    }

    // Don't initialize grid here - wait for first market data
    return true;
}

void GridMarketMaker::stop() {
    cancel_grid_orders();
    BasicMarketMaker::stop();
}

void GridMarketMaker::reset() {
    cancel_grid_orders();
    grid_levels_.clear();
    grid_state_ = GridState{};
    grid_stats_ = GridStats{};
    order_to_level_.clear();
    price_history_.clear();
    current_volatility_ = 0.0;
    consecutive_losses_ = 0;
    current_martingale_mult_ = 1.0;
    BasicMarketMaker::reset();
}

void GridMarketMaker::on_stop() {
    cancel_grid_orders();
}

void GridMarketMaker::on_reset() {
    grid_levels_.clear();
    grid_state_ = GridState{};
    grid_stats_ = GridStats{};
}

// ============================================================================
// Market Data Handler
// ============================================================================

void GridMarketMaker::on_market_data(const MarketData& data) {
    if (!is_running()) {
        return;
    }

    Price mid_price = data.mid_price();
    if (mid_price <= 0) {
        return;
    }

    // Track price history for trend detection
    price_history_.push_back(mid_price);
    while (price_history_.size() > MAX_PRICE_HISTORY) {
        price_history_.pop_front();
    }

    // Initialize grid if needed
    if (grid_levels_.empty()) {
        initialize_grid(mid_price);
        place_grid_orders();
        return;
    }

    // Check if we need to rebalance
    if (should_rebalance(mid_price)) {
        rebalance_grid(mid_price);
        return;
    }

    // Apply anti-trending adjustments if enabled
    if (grid_config_.enable_anti_trending) {
        apply_anti_trending_adjustments();
    }

    // Check profit taking
    if (grid_config_.enable_profit_taking && should_take_profits()) {
        take_profits();
    }

    // Update individual levels as needed
    for (auto& level : grid_levels_) {
        update_grid_level(level, data);
    }

    // Update statistics
    update_grid_stats();

    // Update PnL
    grid_state_.total_grid_pnl = calculate_grid_pnl(mid_price);
}

// ============================================================================
// Order Event Handlers
// ============================================================================

void GridMarketMaker::on_order_update(const OrderUpdate& update) {
    BasicMarketMaker::on_order_update(update);

    // Find the grid level for this order
    auto it = order_to_level_.find(update.order_id);
    if (it != order_to_level_.end()) {
        int level_idx = it->second;
        if (level_idx >= 0 && level_idx < static_cast<int>(grid_levels_.size())) {
            auto& level = grid_levels_[level_idx];

            if (update.status == OrderStatus::CANCELLED ||
                update.status == OrderStatus::REJECTED ||
                update.status == OrderStatus::EXPIRED) {
                level.is_active = false;
                level.order_id = 0;
                order_to_level_.erase(it);
            } else if (update.status == OrderStatus::FILLED) {
                level.is_filled = true;
                level.is_active = false;
                level.times_filled++;
                level.last_fill_time = update.update_timestamp;
                level.average_fill_price = update.average_fill_price;
                order_to_level_.erase(it);

                // Update grid state
                if (level.side == OrderSide::BUY) {
                    grid_state_.filled_buy_levels++;
                    grid_state_.grid_inventory += update.filled_quantity;
                } else {
                    grid_state_.filled_sell_levels++;
                    grid_state_.grid_inventory -= update.filled_quantity;
                }
            }
        }
    }

    // Update active level counts
    grid_state_.active_buy_levels = 0;
    grid_state_.active_sell_levels = 0;
    for (const auto& level : grid_levels_) {
        if (level.is_active) {
            if (level.side == OrderSide::BUY) {
                grid_state_.active_buy_levels++;
            } else {
                grid_state_.active_sell_levels++;
            }
        }
    }
}

void GridMarketMaker::on_trade(const Trade& trade) {
    BasicMarketMaker::on_trade(trade);

    // Find the level and record PnL
    GridLevel* level = get_level_by_order(trade.order_id);
    if (level) {
        on_level_fill(*level, trade);
    }
}

// ============================================================================
// Grid Management
// ============================================================================

void GridMarketMaker::initialize_grid(Price center_price) {
    grid_levels_.clear();
    order_to_level_.clear();

    grid_state_.center_price = center_price;
    grid_state_.initial_center = center_price;

    // Calculate boundaries
    grid_state_.upper_boundary = center_price * (1.0 + grid_config_.grid_upper_bound_pct);
    grid_state_.lower_boundary = center_price * (1.0 - grid_config_.grid_lower_bound_pct);

    int num_levels = grid_config_.num_grid_levels;

    // Create buy levels (below center)
    for (int i = 1; i <= num_levels; ++i) {
        GridLevel level;
        level.level_index = -i;  // Negative for buy levels
        level.price = calculate_level_price(center_price, -i);
        level.size = calculate_level_size(-i);
        level.side = OrderSide::BUY;
        level.is_active = false;
        level.is_filled = false;

        if (level.price >= grid_state_.lower_boundary) {
            grid_levels_.push_back(level);
        }
    }

    // Create sell levels (above center)
    for (int i = 1; i <= num_levels; ++i) {
        GridLevel level;
        level.level_index = i;  // Positive for sell levels
        level.price = calculate_level_price(center_price, i);
        level.size = calculate_level_size(i);
        level.side = OrderSide::SELL;
        level.is_active = false;
        level.is_filled = false;

        if (level.price <= grid_state_.upper_boundary) {
            grid_levels_.push_back(level);
        }
    }

    // Sort levels by price (ascending)
    std::sort(grid_levels_.begin(), grid_levels_.end(),
              [](const GridLevel& a, const GridLevel& b) {
                  return a.price < b.price;
              });

    grid_stats_.total_levels = static_cast<int>(grid_levels_.size());
}

void GridMarketMaker::recalculate_grid() {
    if (grid_state_.center_price <= 0) {
        return;
    }

    // Recalculate prices for all levels
    for (auto& level : grid_levels_) {
        level.price = calculate_level_price(grid_state_.center_price, level.level_index);
        level.size = calculate_level_size(level.level_index);
    }

    // Update boundaries
    grid_state_.upper_boundary = grid_state_.center_price *
                                 (1.0 + grid_config_.grid_upper_bound_pct);
    grid_state_.lower_boundary = grid_state_.center_price *
                                 (1.0 - grid_config_.grid_lower_bound_pct);
}

bool GridMarketMaker::should_rebalance(Price current_price) const {
    if (grid_config_.rebalance_mode == GridRebalanceMode::NONE) {
        return false;
    }

    Price center = grid_state_.center_price;
    if (center <= 0) {
        return false;
    }

    double move_pct = std::abs(current_price - center) / center;

    switch (grid_config_.rebalance_mode) {
        case GridRebalanceMode::ON_BOUNDARY:
            // Rebalance when price hits grid boundary
            return current_price <= grid_state_.lower_boundary ||
                   current_price >= grid_state_.upper_boundary;

        case GridRebalanceMode::PERIODIC: {
            auto now = current_timestamp();
            auto elapsed = (now - grid_state_.last_rebalance).count() / 1000000;
            return elapsed >= grid_config_.rebalance_interval_ms;
        }

        case GridRebalanceMode::ADAPTIVE:
            // Rebalance based on price move and expected profit
            if (move_pct >= grid_config_.rebalance_threshold_pct) {
                double expected_profit = calculate_expected_grid_profit();
                return expected_profit > grid_config_.rebalance_cost_threshold;
            }
            return false;

        default:
            return false;
    }
}

void GridMarketMaker::rebalance_grid(Price new_center) {
    // Cancel all existing orders
    cancel_grid_orders();

    // Reset filled status but preserve PnL
    for (auto& level : grid_levels_) {
        level.is_filled = false;
        level.is_active = false;
        level.order_id = 0;
    }

    // Reinitialize grid around new center
    initialize_grid(new_center);

    // Place new orders
    place_grid_orders();

    // Update state
    grid_state_.last_rebalance = current_timestamp();
    grid_state_.rebalance_count++;
}

void GridMarketMaker::place_grid_orders() {
    for (auto& level : grid_levels_) {
        if (!level.is_active && !level.is_filled &&
            level.size >= grid_config_.min_order_size) {

            // Generate order signal
            generate_order_signal(level.side, level.price, level.size);

            level.is_active = true;
            // Order ID will be set when we receive order confirmation

            // Update counts
            if (level.side == OrderSide::BUY) {
                grid_state_.active_buy_levels++;
            } else {
                grid_state_.active_sell_levels++;
            }
        }
    }
}

void GridMarketMaker::cancel_grid_orders() {
    for (auto& level : grid_levels_) {
        if (level.is_active && level.order_id > 0) {
            // Cancel order
            Signal cancel_signal;
            cancel_signal.type = (level.side == OrderSide::BUY) ?
                                SignalType::CLOSE_LONG : SignalType::CLOSE_SHORT;
            cancel_signal.symbol = config_.symbols.empty() ? "" : config_.symbols[0];
            cancel_signal.target_quantity = 0;
            emit_signal(std::move(cancel_signal));

            level.is_active = false;
            level.order_id = 0;
        }
    }

    grid_state_.active_buy_levels = 0;
    grid_state_.active_sell_levels = 0;
    order_to_level_.clear();
}

void GridMarketMaker::update_grid_level(GridLevel& level, const MarketData& data) {
    // If level was filled, check if we should place the opposite order
    if (level.is_filled && !level.is_active) {
        // The level was filled - in grid trading, we typically place
        // the opposite order at the corresponding level
        // This is handled by checking if price has moved back

        Price current_mid = data.mid_price();

        // For buy levels that were filled, check if price is now above
        if (level.side == OrderSide::BUY && current_mid > level.price) {
            // Price moved up after our buy - could place sell at higher level
            // This is handled by sell levels already in the grid
        }

        // Reset the level for potential reuse
        if ((level.side == OrderSide::BUY && current_mid > level.price * 1.01) ||
            (level.side == OrderSide::SELL && current_mid < level.price * 0.99)) {
            level.is_filled = false;  // Allow level to be used again
        }
    }
}

// ============================================================================
// Grid Calculation
// ============================================================================

Price GridMarketMaker::calculate_level_price(Price center_price, int level_index) const {
    if (level_index == 0) {
        return center_price;
    }

    double spacing;

    switch (grid_config_.spacing_type) {
        case GridSpacingType::ARITHMETIC:
            // Linear spacing: price = center + level * spacing
            spacing = (grid_config_.grid_spacing_bps / 10000.0) * center_price;
            return center_price + level_index * spacing;

        case GridSpacingType::GEOMETRIC:
            // Percentage spacing: price = center * (1 + pct)^level
            return center_price * std::pow(1.0 + grid_config_.grid_spacing_pct, level_index);

        case GridSpacingType::DYNAMIC:
            // Volatility-adjusted spacing
            spacing = calculate_dynamic_spacing(current_volatility_);
            return center_price * std::pow(1.0 + spacing, level_index);

        default:
            return center_price;
    }
}

Quantity GridMarketMaker::calculate_level_size(int level_index) const {
    double base_size = grid_config_.base_level_size;
    int abs_level = std::abs(level_index);

    // Apply size multiplier for outer levels
    double size = base_size * std::pow(grid_config_.size_multiplier, abs_level);

    // Apply martingale if enabled
    if (grid_config_.use_martingale) {
        size *= current_martingale_mult_;
    }

    // Apply limits
    size = std::clamp(size, grid_config_.min_order_size, grid_config_.max_level_size);

    return round_quantity(size);
}

double GridMarketMaker::calculate_dynamic_spacing(double volatility) const {
    // Base spacing
    double base_spacing = grid_config_.grid_spacing_pct;

    // Adjust based on volatility
    double vol_clamped = std::clamp(volatility,
                                    grid_config_.min_volatility_for_spacing,
                                    grid_config_.max_volatility_for_spacing);

    double vol_ratio = vol_clamped / grid_config_.min_volatility_for_spacing;
    double adjusted = base_spacing * (1.0 + grid_config_.volatility_spacing_mult * (vol_ratio - 1.0));

    // Clamp to limits
    double min_spacing = grid_config_.min_grid_spacing_bps / 10000.0;
    double max_spacing = grid_config_.max_grid_spacing_bps / 10000.0;

    return std::clamp(adjusted, min_spacing, max_spacing);
}

double GridMarketMaker::calculate_expected_grid_profit() const {
    // Calculate expected profit from grid based on current structure
    double expected = 0.0;

    // Count potential round trips
    int buy_levels_active = 0;
    int sell_levels_active = 0;

    for (const auto& level : grid_levels_) {
        if (level.side == OrderSide::BUY && (level.is_active || !level.is_filled)) {
            buy_levels_active++;
        } else if (level.side == OrderSide::SELL && (level.is_active || !level.is_filled)) {
            sell_levels_active++;
        }
    }

    // Each round trip captures approximately the grid spacing
    int potential_trips = std::min(buy_levels_active, sell_levels_active);
    double avg_spacing = (grid_config_.grid_spacing_bps / 10000.0) * grid_state_.center_price;

    expected = potential_trips * avg_spacing * grid_config_.base_level_size;

    return expected;
}

// ============================================================================
// Trend Detection
// ============================================================================

int GridMarketMaker::detect_trend() const {
    if (price_history_.size() < static_cast<size_t>(grid_config_.trend_detection_bars)) {
        return 0;  // Not enough data
    }

    // Simple trend detection: compare recent prices to older prices
    size_t bars = grid_config_.trend_detection_bars;
    Price recent_avg = 0.0;
    Price older_avg = 0.0;

    for (size_t i = price_history_.size() - bars/2; i < price_history_.size(); ++i) {
        recent_avg += price_history_[i];
    }
    recent_avg /= (bars/2);

    for (size_t i = price_history_.size() - bars; i < price_history_.size() - bars/2; ++i) {
        older_avg += price_history_[i];
    }
    older_avg /= (bars/2);

    double move_bps = ((recent_avg - older_avg) / older_avg) * 10000.0;

    if (move_bps > grid_config_.trend_threshold_bps) {
        return 1;  // Uptrend
    } else if (move_bps < -grid_config_.trend_threshold_bps) {
        return -1;  // Downtrend
    }

    return 0;  // Ranging
}

double GridMarketMaker::calculate_trend_strength() const {
    int trend = detect_trend();
    if (trend == 0) {
        return 0.0;
    }

    // Calculate strength based on move magnitude
    if (price_history_.size() < 2) {
        return 0.0;
    }

    Price start = price_history_[price_history_.size() - grid_config_.trend_detection_bars];
    Price end = price_history_.back();

    double move_pct = std::abs(end - start) / start;
    double threshold_pct = grid_config_.trend_threshold_bps / 10000.0;

    return std::min(1.0, move_pct / threshold_pct - 1.0);
}

void GridMarketMaker::apply_anti_trending_adjustments() {
    int trend = detect_trend();

    grid_state_.is_trending_up = (trend > 0);
    grid_state_.is_trending_down = (trend < 0);

    if (trend == 0) {
        return;  // No trend, no adjustment
    }

    // Reduce size in trend direction
    for (auto& level : grid_levels_) {
        if ((trend > 0 && level.side == OrderSide::SELL) ||
            (trend < 0 && level.side == OrderSide::BUY)) {
            // This side is against the trend - reduce size
            level.size *= grid_config_.anti_trend_size_reduction;
        }
    }
}

// ============================================================================
// Profit Management
// ============================================================================

bool GridMarketMaker::should_take_profits() const {
    return grid_state_.total_grid_pnl >= grid_config_.profit_take_threshold;
}

void GridMarketMaker::take_profits() {
    // Close partial position to take profits
    double profit_to_take = grid_state_.total_grid_pnl * grid_config_.profit_take_ratio;

    // Generate signal to reduce position
    Signal signal;
    signal.symbol = config_.symbols.empty() ? "" : config_.symbols[0];
    signal.confidence = 0.8;
    signal.urgency = 0.5;

    if (grid_state_.grid_inventory > 0) {
        signal.type = SignalType::REDUCE_POSITION;
        signal.target_quantity = grid_state_.grid_inventory * grid_config_.profit_take_ratio;
    } else if (grid_state_.grid_inventory < 0) {
        signal.type = SignalType::REDUCE_POSITION;
        signal.target_quantity = -grid_state_.grid_inventory * grid_config_.profit_take_ratio;
    }

    if (signal.target_quantity > grid_config_.min_order_size) {
        emit_signal(std::move(signal));
    }
}

double GridMarketMaker::calculate_grid_pnl(Price current_price) const {
    double pnl = 0.0;

    // Calculate unrealized PnL from current inventory
    pnl += grid_state_.grid_inventory * (current_price - grid_state_.center_price);

    // Add realized PnL from filled levels
    for (const auto& level : grid_levels_) {
        pnl += level.level_pnl;
    }

    return pnl;
}

// ============================================================================
// Grid Level Access
// ============================================================================

const GridLevel* GridMarketMaker::get_level(int index) const {
    for (const auto& level : grid_levels_) {
        if (level.level_index == index) {
            return &level;
        }
    }
    return nullptr;
}

GridLevel* GridMarketMaker::get_level_by_order(uint64_t order_id) {
    auto it = order_to_level_.find(order_id);
    if (it != order_to_level_.end()) {
        int idx = it->second;
        if (idx >= 0 && idx < static_cast<int>(grid_levels_.size())) {
            return &grid_levels_[idx];
        }
    }
    return nullptr;
}

// ============================================================================
// Internal Methods
// ============================================================================

void GridMarketMaker::on_level_fill(GridLevel& level, const Trade& fill) {
    // Calculate PnL for this fill
    double fill_pnl = 0.0;

    if (level.side == OrderSide::SELL) {
        // Sold - profit if price above center
        fill_pnl = (fill.price - grid_state_.center_price) * fill.quantity;
    } else {
        // Bought - will profit when we sell above this price
        // For now, record the potential
        fill_pnl = 0;  // Unrealized until sold
    }

    level.level_pnl += fill_pnl;
    grid_stats_.total_pnl += fill_pnl;
    grid_stats_.total_volume += fill.quantity;
    grid_stats_.total_fills++;

    // Update fill time
    if (grid_stats_.first_fill_time.count() == 0) {
        grid_stats_.first_fill_time = fill.timestamp;
    }
    grid_stats_.last_fill_time = fill.timestamp;

    // Update martingale if used
    if (grid_config_.use_martingale) {
        if (fill_pnl < 0) {
            consecutive_losses_++;
            current_martingale_mult_ *= grid_config_.martingale_multiplier;
        } else {
            consecutive_losses_ = 0;
            current_martingale_mult_ = 1.0;
        }
    }

    // Update statistics
    if (fill_pnl > grid_stats_.max_level_pnl) {
        grid_stats_.max_level_pnl = fill_pnl;
    }
    if (fill_pnl < grid_stats_.min_level_pnl) {
        grid_stats_.min_level_pnl = fill_pnl;
    }
}

void GridMarketMaker::update_grid_stats() {
    grid_stats_.active_levels = 0;
    grid_stats_.filled_levels = 0;

    for (const auto& level : grid_levels_) {
        if (level.is_active) {
            grid_stats_.active_levels++;
        }
        if (level.is_filled) {
            grid_stats_.filled_levels++;
        }
    }

    // Calculate fill rate
    if (grid_stats_.total_levels > 0) {
        grid_stats_.fill_rate = static_cast<double>(grid_stats_.filled_levels) /
                               grid_stats_.total_levels;
    }

    // Calculate average PnL per fill
    if (grid_stats_.total_fills > 0) {
        grid_stats_.average_pnl_per_fill = grid_stats_.total_pnl / grid_stats_.total_fills;
    }

    // Calculate spread coverage
    if (grid_state_.center_price > 0) {
        double range = grid_state_.upper_boundary - grid_state_.lower_boundary;
        grid_stats_.current_spread_coverage = range / grid_state_.center_price;
    }
}

void GridMarketMaker::expand_grid(int direction) {
    if (!grid_config_.auto_expand_grid) {
        return;
    }

    int num_levels = grid_config_.num_grid_levels;
    Price center = grid_state_.center_price;

    if (direction > 0) {
        // Expand upward
        int current_max = 0;
        for (const auto& level : grid_levels_) {
            if (level.level_index > current_max) {
                current_max = level.level_index;
            }
        }

        for (int i = current_max + 1; i <= current_max + num_levels / 2; ++i) {
            GridLevel level;
            level.level_index = i;
            level.price = calculate_level_price(center, i);
            level.size = calculate_level_size(i);
            level.side = OrderSide::SELL;

            if (level.price <= grid_state_.upper_boundary * 1.5) {
                grid_levels_.push_back(level);
            }
        }
    } else {
        // Expand downward
        int current_min = 0;
        for (const auto& level : grid_levels_) {
            if (level.level_index < current_min) {
                current_min = level.level_index;
            }
        }

        for (int i = current_min - 1; i >= current_min - num_levels / 2; --i) {
            GridLevel level;
            level.level_index = i;
            level.price = calculate_level_price(center, i);
            level.size = calculate_level_size(i);
            level.side = OrderSide::BUY;

            if (level.price >= grid_state_.lower_boundary * 0.5) {
                grid_levels_.push_back(level);
            }
        }
    }

    // Re-sort levels
    std::sort(grid_levels_.begin(), grid_levels_.end(),
              [](const GridLevel& a, const GridLevel& b) {
                  return a.price < b.price;
              });

    grid_stats_.total_levels = static_cast<int>(grid_levels_.size());
}

void GridMarketMaker::contract_grid() {
    // Remove outer levels that haven't been used
    auto it = std::remove_if(grid_levels_.begin(), grid_levels_.end(),
        [](const GridLevel& level) {
            return std::abs(level.level_index) > 5 &&
                   level.times_filled == 0 &&
                   !level.is_active;
        });

    if (it != grid_levels_.end()) {
        grid_levels_.erase(it, grid_levels_.end());
        grid_stats_.total_levels = static_cast<int>(grid_levels_.size());
    }
}

}  // namespace market_making
}  // namespace strategies
}  // namespace hft
