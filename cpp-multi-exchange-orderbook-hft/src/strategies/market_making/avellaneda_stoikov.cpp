/**
 * @file avellaneda_stoikov.cpp
 * @brief Implementation of Avellaneda-Stoikov Optimal Market Making Strategy
 *
 * This implements the optimal market making model from:
 * Avellaneda, M., & Stoikov, S. (2008). "High-frequency trading in a limit order book."
 * Quantitative Finance, 8(3), 217-224.
 *
 * Key formulas:
 * 1. Reservation price: r(s,q,t) = s - q * gamma * sigma^2 * (T - t)
 * 2. Optimal spread: delta = gamma * sigma^2 * (T - t) + (2/gamma) * ln(1 + gamma/k)
 * 3. Order intensity: Lambda(delta) = A * exp(-k * delta)
 */

#include "strategies/market_making/avellaneda_stoikov.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace hft {
namespace strategies {
namespace market_making {

// ============================================================================
// Constructor
// ============================================================================

AvellanedaStoikov::AvellanedaStoikov(StrategyConfig config, AvellanedaStoikovConfig as_config)
    : BasicMarketMaker(std::move(config), as_config)
    , as_config_(std::move(as_config))
{
    mm_config_ = as_config_;
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

bool AvellanedaStoikov::initialize() {
    if (!BasicMarketMaker::initialize()) {
        return false;
    }

    // Initialize model state
    model_state_ = ASModelState{};
    model_state_.T = as_config_.T_seconds;
    model_state_.tau = as_config_.T_seconds;
    model_state_.sigma = as_config_.sigma;
    model_state_.sigma_squared = as_config_.sigma * as_config_.sigma;

    // Initialize volatility estimation
    volatility_estimate_ = VolatilityEstimate{};

    // Initialize metrics
    as_metrics_ = ASMetrics{};

    // Clear history
    price_returns_.clear();
    drift_samples_.clear();

    // Initialize EWMA
    ewma_variance_ = as_config_.sigma * as_config_.sigma;
    last_price_for_vol_ = 0.0;

    // Initialize period tracking
    period_start_time_ = current_timestamp();
    period_start_inventory_ = 0.0;
    periods_completed_ = 0;

    // Jump detection
    recent_max_move_ = 0.0;
    jump_detected_ = false;

    return true;
}

bool AvellanedaStoikov::start() {
    if (!BasicMarketMaker::start()) {
        return false;
    }

    // Reset period
    reset_period();

    return true;
}

void AvellanedaStoikov::reset() {
    BasicMarketMaker::reset();
    on_reset();
}

void AvellanedaStoikov::on_reset() {
    model_state_ = ASModelState{};
    model_state_.T = as_config_.T_seconds;
    model_state_.tau = as_config_.T_seconds;
    model_state_.sigma = as_config_.sigma;

    volatility_estimate_ = VolatilityEstimate{};
    as_metrics_ = ASMetrics{};
    price_returns_.clear();
    drift_samples_.clear();
    ewma_variance_ = as_config_.sigma * as_config_.sigma;
    last_price_for_vol_ = 0.0;
    periods_completed_ = 0;
}

// ============================================================================
// Market Data Handler
// ============================================================================

void AvellanedaStoikov::on_market_data(const MarketData& data) {
    if (!is_running()) {
        return;
    }

    Price mid_price = data.mid_price();
    if (mid_price <= 0) {
        return;
    }

    // Update volatility estimate
    update_volatility(mid_price, data.timestamp);

    // Update model state
    update_model_state(data);

    // Compute model
    compute_model();

    // Check for period end
    if (in_liquidation_period()) {
        handle_period_end();
    }

    // Update metrics
    update_as_metrics();

    // Call base class (uses our overridden quote calculation)
    BasicMarketMaker::on_market_data(data);
}

// ============================================================================
// Order Event Handlers
// ============================================================================

void AvellanedaStoikov::on_order_update(const OrderUpdate& update) {
    BasicMarketMaker::on_order_update(update);

    // Track fill rates for model calibration
    if (update.status == OrderStatus::FILLED) {
        if (update.side == OrderSide::BUY) {
            as_metrics_.bid_fills++;
        } else {
            as_metrics_.ask_fills++;
        }
    }
}

void AvellanedaStoikov::on_trade(const Trade& trade) {
    BasicMarketMaker::on_trade(trade);

    // Update PnL breakdown
    double half_spread = model_state_.optimal_spread / 2.0;
    as_metrics_.spread_pnl += half_spread * trade.quantity;
}

// ============================================================================
// Core A-S Model Implementation
// ============================================================================

Price AvellanedaStoikov::calculate_reservation_price(
    Price mid_price,
    double inventory,
    double time_remaining) const {
    /**
     * Reservation price formula:
     * r(s,q,t) = s - q * gamma * sigma^2 * (T - t)
     *
     * Interpretation:
     * - When inventory q > 0 (long), reservation price < mid (want to sell)
     * - When inventory q < 0 (short), reservation price > mid (want to buy)
     * - The adjustment scales with:
     *   - gamma: risk aversion (higher = more aggressive adjustment)
     *   - sigma^2: variance (higher vol = larger adjustment)
     *   - (T-t): time remaining (more time = larger adjustment)
     */

    double gamma = as_config_.gamma;
    double sigma_sq = model_state_.sigma_squared;
    double tau = std::max(time_remaining, MIN_TAU);

    // Inventory adjustment
    double inventory_adjustment = inventory * gamma * sigma_sq * tau;

    // Calculate reservation price
    Price reservation = mid_price - inventory_adjustment;

    return reservation;
}

double AvellanedaStoikov::calculate_optimal_spread(double time_remaining) const {
    /**
     * Optimal spread formula:
     * delta(q,t) = gamma * sigma^2 * (T - t) + (2/gamma) * ln(1 + gamma/k)
     *
     * The spread has two components:
     * 1. Risk component: gamma * sigma^2 * tau
     *    - Increases with volatility and time remaining
     *    - Compensates for inventory risk
     *
     * 2. Intensity component: (2/gamma) * ln(1 + gamma/k)
     *    - Based on order arrival intensity
     *    - k = intensity parameter (higher k = more liquid)
     *    - This is the "adverse selection" component
     */

    double gamma = as_config_.gamma;
    double sigma_sq = model_state_.sigma_squared;
    double k = as_config_.k;
    double tau = std::max(time_remaining, MIN_TAU);

    // Risk component
    double risk_component = gamma * sigma_sq * tau;

    // Intensity component
    double intensity_component = (2.0 / gamma) * std::log(1.0 + gamma / k);

    // Total spread
    double spread = risk_component + intensity_component;

    // Convert to price units (spread is in price terms, not percentage)
    // For the formula above, if sigma is in percentage terms per sqrt(time),
    // we need to scale appropriately

    // The spread is typically a small percentage of price
    // Scale to match price level
    spread = spread * model_state_.mid_price;

    return spread;
}

double AvellanedaStoikov::calculate_intensity(double spread_half) const {
    /**
     * Order arrival intensity models:
     *
     * 1. Exponential: Lambda = A * exp(-k * delta)
     *    - Standard A-S assumption
     *    - delta = distance from fair price
     *
     * 2. Power law: Lambda = A / (1 + k * delta)^alpha
     *    - Fat tails in arrival distribution
     *
     * 3. Linear: Lambda = max(0, A - k * delta)
     *    - Simple approximation
     */

    double A = as_config_.intensity_A;
    double k = as_config_.k;

    switch (as_config_.intensity_func) {
        case IntensityFunction::CONSTANT:
            return k;

        case IntensityFunction::LINEAR_SPREAD:
        case IntensityFunction::EXPONENTIAL:
            return A * std::exp(-k * spread_half);

        case IntensityFunction::POWER_LAW: {
            double alpha = as_config_.intensity_alpha;
            return A / std::pow(1.0 + k * spread_half, alpha);
        }

        default:
            return A * std::exp(-k * spread_half);
    }
}

std::pair<Price, Price> AvellanedaStoikov::calculate_optimal_quotes() const {
    /**
     * Optimal quotes are placed symmetrically around reservation price:
     *
     * bid = r - delta/2
     * ask = r + delta/2
     *
     * where r is reservation price and delta is optimal spread
     */

    Price r = model_state_.reservation_price;
    double half_spread = model_state_.optimal_spread / 2.0;

    // Apply minimum spread constraint
    double min_spread = (as_config_.min_spread_bps / 10000.0) * model_state_.mid_price;
    half_spread = std::max(half_spread, min_spread / 2.0);

    // Apply maximum spread constraint
    double max_spread = (as_config_.max_spread_bps / 10000.0) * model_state_.mid_price;
    half_spread = std::min(half_spread, max_spread / 2.0);

    Price bid = r - half_spread;
    Price ask = r + half_spread;

    // Apply urgency adjustment near period end
    if (as_config_.enable_end_of_period) {
        double urgency = calculate_urgency();
        if (urgency > 1.0) {
            // Reduce spread to increase fill probability
            double spread_reduction = 1.0 / urgency;
            double current_spread = ask - bid;
            double new_spread = current_spread * spread_reduction;
            double spread_diff = (current_spread - new_spread) / 2.0;
            bid += spread_diff;
            ask -= spread_diff;
        }
    }

    // Apply drift adjustment if enabled
    if (as_config_.enable_drift_adjustment) {
        std::tie(bid, ask) = adjust_for_drift(bid, ask);
    }

    return {bid, ask};
}

void AvellanedaStoikov::update_model_state(const MarketData& data) {
    // Update time
    auto now = data.timestamp;
    auto elapsed = (now - period_start_time_).count() / 1e9;  // seconds
    model_state_.t = elapsed;
    model_state_.tau = std::max(model_state_.T - elapsed, MIN_TAU);

    // Update price
    model_state_.mid_price = data.mid_price();

    // Use microprice if enabled and available
    if (as_config_.use_microprice && data.orderbook.bid_levels > 0 &&
        data.orderbook.ask_levels > 0) {
        double bid = data.orderbook.bids[0].price;
        double ask = data.orderbook.asks[0].price;
        double bid_qty = data.orderbook.bids[0].quantity;
        double ask_qty = data.orderbook.asks[0].quantity;

        if (bid_qty + ask_qty > 0) {
            model_state_.microprice = (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty);
        } else {
            model_state_.microprice = model_state_.mid_price;
        }
    } else {
        model_state_.microprice = model_state_.mid_price;
    }

    // Update inventory
    for (const auto& [sym, pos] : positions_) {
        model_state_.inventory = pos.quantity;
        break;
    }

    // Update volatility
    model_state_.sigma = volatility_estimate_.current_vol;
    model_state_.sigma_squared = model_state_.sigma * model_state_.sigma;
}

void AvellanedaStoikov::compute_model() {
    // Use microprice or mid price as reference
    Price reference = as_config_.use_microprice ?
                     model_state_.microprice : model_state_.mid_price;

    // Calculate reservation price
    model_state_.reservation_price = calculate_reservation_price(
        reference,
        model_state_.inventory,
        model_state_.tau
    );

    // Calculate optimal spread
    model_state_.optimal_spread = calculate_optimal_spread(model_state_.tau);

    // Calculate optimal quotes
    auto [bid, ask] = calculate_optimal_quotes();
    model_state_.optimal_bid = bid;
    model_state_.optimal_ask = ask;

    // Calculate intensities
    double half_spread = model_state_.optimal_spread / 2.0;
    model_state_.lambda_bid = calculate_intensity(half_spread);
    model_state_.lambda_ask = calculate_intensity(half_spread);

    // Risk components
    model_state_.inventory_risk = std::abs(model_state_.inventory) *
                                  as_config_.gamma *
                                  model_state_.sigma_squared *
                                  model_state_.tau;

    model_state_.spread_risk_component = as_config_.gamma *
                                         model_state_.sigma_squared *
                                         model_state_.tau;

    model_state_.spread_intensity_component = (2.0 / as_config_.gamma) *
                                              std::log(1.0 + as_config_.gamma / as_config_.k);
}

// ============================================================================
// Volatility Estimation
// ============================================================================

void AvellanedaStoikov::update_volatility(Price price, Timestamp timestamp) {
    if (last_price_for_vol_ <= 0) {
        last_price_for_vol_ = price;
        last_vol_update_ = timestamp;
        return;
    }

    // Calculate log return
    double log_return = std::log(price / last_price_for_vol_);

    // Store return
    PriceReturn pr;
    pr.price = price;
    pr.log_return = log_return;
    pr.timestamp = timestamp;
    price_returns_.push_back(pr);

    // Trim old returns
    while (!price_returns_.empty() &&
           (timestamp - price_returns_.front().timestamp).count() >
           static_cast<int64_t>(as_config_.volatility_lookback_seconds * 1e9)) {
        price_returns_.pop_front();
    }

    // Calculate EWMA volatility
    volatility_estimate_.ewma_vol = calculate_ewma_volatility(log_return);

    // Calculate realized volatility
    volatility_estimate_.realized_vol = calculate_realized_volatility();

    // Detect jumps
    if (as_config_.enable_jump_detection) {
        double move = std::abs(log_return);
        recent_max_move_ = std::max(recent_max_move_ * 0.99, move);
        jump_detected_ = detect_jump(move);
    }

    // Select current volatility
    if (as_config_.use_realized_volatility && price_returns_.size() > 10) {
        volatility_estimate_.current_vol = volatility_estimate_.realized_vol;
    } else {
        volatility_estimate_.current_vol = volatility_estimate_.ewma_vol;
    }

    // Clamp to limits
    volatility_estimate_.current_vol = std::clamp(
        volatility_estimate_.current_vol,
        as_config_.min_sigma,
        as_config_.max_sigma
    );

    volatility_estimate_.sample_count = static_cast<int>(price_returns_.size());
    volatility_estimate_.last_update = timestamp;

    // Update for next iteration
    last_price_for_vol_ = price;
    last_vol_update_ = timestamp;

    // Store for drift calculation
    if (as_config_.enable_drift_adjustment) {
        drift_samples_.push_back({price, timestamp});
        while (!drift_samples_.empty() &&
               (timestamp - drift_samples_.front().second).count() >
               static_cast<int64_t>(as_config_.drift_lookback_seconds * 1e9)) {
            drift_samples_.pop_front();
        }
    }
}

double AvellanedaStoikov::calculate_realized_volatility() const {
    /**
     * Realized volatility using close-to-close returns:
     * sigma = sqrt(sum(r^2) / n) * sqrt(periods_per_year)
     */

    if (price_returns_.size() < 2) {
        return as_config_.sigma;
    }

    double sum_squared = 0.0;
    int count = 0;

    for (const auto& pr : price_returns_) {
        if (pr.log_return != 0.0) {
            sum_squared += pr.log_return * pr.log_return;
            count++;
        }
    }

    if (count == 0) {
        return as_config_.sigma;
    }

    double variance = sum_squared / count;

    // Annualize based on sampling frequency
    auto time_span = price_returns_.back().timestamp - price_returns_.front().timestamp;
    double seconds = time_span.count() / 1e9;

    if (seconds > 0) {
        double samples_per_second = count / seconds;
        // Convert to annualized volatility
        return std::sqrt(variance * samples_per_second * SECONDS_PER_YEAR);
    }

    return std::sqrt(variance);
}

double AvellanedaStoikov::calculate_ewma_volatility(double new_return) {
    /**
     * EWMA Volatility:
     * sigma^2_t = decay * sigma^2_{t-1} + (1-decay) * r^2_t
     */

    double decay = as_config_.volatility_ewma_decay;
    double squared_return = new_return * new_return;

    // EWMA update
    ewma_variance_ = decay * ewma_variance_ + (1.0 - decay) * squared_return;

    // Return annualized volatility
    // Assuming returns are at some frequency, annualize based on time between samples
    double dt = 1.0;  // Default to 1 second
    if (price_returns_.size() >= 2) {
        auto time_diff = price_returns_.back().timestamp -
                        price_returns_[price_returns_.size()-2].timestamp;
        dt = time_diff.count() / 1e9;
        if (dt <= 0) dt = 1.0;
    }

    double periods_per_year = SECONDS_PER_YEAR / dt;
    return std::sqrt(ewma_variance_ * periods_per_year);
}

// ============================================================================
// Time Management
// ============================================================================

void AvellanedaStoikov::reset_period() {
    period_start_time_ = current_timestamp();
    period_start_inventory_ = model_state_.inventory;

    // Reset model state time
    model_state_.t = 0.0;
    model_state_.tau = as_config_.T_seconds;
}

bool AvellanedaStoikov::in_liquidation_period() const {
    if (!as_config_.enable_end_of_period) {
        return false;
    }

    double progress = model_state_.t / model_state_.T;
    return progress >= as_config_.liquidation_threshold;
}

double AvellanedaStoikov::calculate_urgency() const {
    /**
     * Urgency increases as we approach period end and have inventory.
     * urgency = end_of_period_urgency * (1 - tau/T) * |inventory|/max_inventory
     */

    double progress = model_state_.t / model_state_.T;
    double inv_ratio = std::abs(model_state_.inventory) / as_config_.max_inventory_units;
    inv_ratio = std::min(inv_ratio, 1.0);

    if (progress < as_config_.liquidation_threshold) {
        return 1.0;  // Normal urgency
    }

    // Increase urgency as we approach T
    double time_urgency = (progress - as_config_.liquidation_threshold) /
                         (1.0 - as_config_.liquidation_threshold);

    return 1.0 + as_config_.end_of_period_urgency * time_urgency * inv_ratio;
}

// ============================================================================
// Drift and Jump Handling
// ============================================================================

double AvellanedaStoikov::estimate_drift() const {
    if (drift_samples_.size() < 2) {
        return 0.0;
    }

    // Linear regression to estimate drift
    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
    int n = 0;

    auto t0 = drift_samples_.front().second;

    for (const auto& [price, time] : drift_samples_) {
        double x = (time - t0).count() / 1e9;  // Time in seconds
        double y = std::log(price / drift_samples_.front().first);

        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
        n++;
    }

    if (n < 2) {
        return 0.0;
    }

    // Slope of regression = drift per second
    double denom = n * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-10) {
        return 0.0;
    }

    double drift = (n * sum_xy - sum_x * sum_y) / denom;
    return drift;
}

bool AvellanedaStoikov::detect_jump(double price_move) const {
    double threshold = as_config_.jump_threshold_sigma * volatility_estimate_.current_vol;

    // Scale threshold for sampling frequency
    double dt = 1.0;
    if (price_returns_.size() >= 2) {
        auto time_diff = price_returns_.back().timestamp -
                        price_returns_[price_returns_.size()-2].timestamp;
        dt = time_diff.count() / 1e9;
        if (dt <= 0) dt = 1.0;
    }

    // Adjust threshold for time scale
    threshold *= std::sqrt(dt / SECONDS_PER_YEAR);

    return std::abs(price_move) > threshold;
}

std::pair<Price, Price> AvellanedaStoikov::adjust_for_drift(
    Price bid, Price ask) const {

    double drift = estimate_drift();

    if (std::abs(drift) < 1e-10) {
        return {bid, ask};
    }

    // Adjust quotes in direction of drift
    // If drift > 0 (price rising), raise both quotes
    // If drift < 0 (price falling), lower both quotes

    // Scale drift for time until next quote update
    double quote_interval_seconds = as_config_.quote_update_interval_ms / 1000.0;
    double expected_move = drift * quote_interval_seconds * model_state_.mid_price;

    // Apply fraction of expected move
    double adjustment = expected_move * 0.5;  // Conservative 50%

    return {bid + adjustment, ask + adjustment};
}

// ============================================================================
// Quote Calculation Override
// ============================================================================

std::pair<Price, Price> AvellanedaStoikov::calculate_quotes(Price mid_price) const {
    // Use the optimal quotes computed by the model
    Price bid = model_state_.optimal_bid;
    Price ask = model_state_.optimal_ask;

    // If model hasn't computed quotes yet, fall back to base implementation
    if (bid <= 0 || ask <= 0 || ask <= bid) {
        return BasicMarketMaker::calculate_quotes(mid_price);
    }

    // Round to tick size
    bid = round_price(bid);
    ask = round_price(ask);

    // Ensure proper ordering
    if (bid >= ask) {
        ask = bid + mm_config_.tick_size;
    }

    // Ensure quotes don't cross the market
    // This shouldn't happen with proper model, but safety check
    if (bid >= mid_price) {
        bid = mid_price - mm_config_.tick_size;
    }
    if (ask <= mid_price) {
        ask = mid_price + mm_config_.tick_size;
    }

    return {bid, ask};
}

// ============================================================================
// Internal Methods
// ============================================================================

double AvellanedaStoikov::convert_volatility(
    double vol, double from_seconds, double to_seconds) {
    /**
     * Convert volatility between time scales using square root of time rule:
     * sigma_T = sigma_t * sqrt(T/t)
     */
    return vol * std::sqrt(to_seconds / from_seconds);
}

void AvellanedaStoikov::update_as_metrics() {
    // Update inventory-related metrics
    as_metrics_.average_inventory = (as_metrics_.average_inventory * periods_completed_ +
                                    std::abs(model_state_.inventory)) / (periods_completed_ + 1);

    as_metrics_.max_inventory = std::max(as_metrics_.max_inventory,
                                         std::abs(model_state_.inventory));

    // Time-weighted inventory
    double time_weight = model_state_.t / model_state_.T;
    as_metrics_.inventory_time_weighted = as_metrics_.inventory_time_weighted * (1.0 - time_weight) +
                                          std::abs(model_state_.inventory) * time_weight;

    // Update PnL
    for (const auto& [sym, pos] : positions_) {
        as_metrics_.inventory_pnl = pos.unrealized_pnl;
        break;
    }
    as_metrics_.total_pnl = as_metrics_.spread_pnl + as_metrics_.inventory_pnl +
                           risk_metrics_.realized_pnl;

    // Spread captured
    int total_fills = as_metrics_.bid_fills + as_metrics_.ask_fills;
    if (total_fills > 0) {
        as_metrics_.average_spread_captured = as_metrics_.spread_pnl / total_fills;
    }

    // Fill rate
    double expected_fills = model_state_.lambda_bid + model_state_.lambda_ask;
    if (expected_fills > 0) {
        as_metrics_.expected_fill_rate = (as_metrics_.bid_fills + as_metrics_.ask_fills) /
                                        (expected_fills * model_state_.t);
    }

    as_metrics_.average_fill_rate = quote_stats_.fill_rate;
}

void AvellanedaStoikov::handle_period_end() {
    // If we have inventory and are at period end, try to close it
    double inv = model_state_.inventory;

    if (std::abs(inv) > mm_config_.min_order_size) {
        // Generate aggressive liquidation signal
        Signal signal;
        signal.type = (inv > 0) ? SignalType::SELL : SignalType::BUY;
        signal.symbol = config_.symbols.empty() ? "" : config_.symbols[0];
        signal.target_quantity = std::abs(inv);
        signal.confidence = 1.0;
        signal.urgency = calculate_urgency();

        // Price at or through the market
        if (inv > 0) {
            signal.target_price = model_state_.mid_price - mm_config_.tick_size;
        } else {
            signal.target_price = model_state_.mid_price + mm_config_.tick_size;
        }

        emit_signal(std::move(signal));
    }

    // Check if period fully ended
    if (model_state_.tau <= MIN_TAU) {
        // Period ended - reset for next period
        periods_completed_++;
        as_metrics_.periods_completed = periods_completed_;
        reset_period();
    }
}

void AvellanedaStoikov::set_gamma(double new_gamma) {
    if (new_gamma > 0) {
        as_config_.gamma = new_gamma;
    }
}

}  // namespace market_making
}  // namespace strategies
}  // namespace hft
