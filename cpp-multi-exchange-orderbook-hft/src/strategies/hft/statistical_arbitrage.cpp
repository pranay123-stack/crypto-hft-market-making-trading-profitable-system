#include "strategies/hft/statistical_arbitrage.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace hft {
namespace strategies {

// ============================================================================
// Constructor
// ============================================================================

StatisticalArbitrageStrategy::StatisticalArbitrageStrategy(StrategyConfig config)
    : StrategyBase(std::move(config))
{
    // Load parameters from config
    params_.entry_zscore = config_.get_param<double>("entry_zscore", 2.0);
    params_.exit_zscore = config_.get_param<double>("exit_zscore", 0.5);
    params_.stop_zscore = config_.get_param<double>("stop_zscore", 4.0);
    params_.lookback_period = static_cast<size_t>(
        config_.get_param<double>("lookback_period", 100.0));
    params_.correlation_window = static_cast<size_t>(
        config_.get_param<double>("correlation_window", 50.0));
    params_.cointegration_window = static_cast<size_t>(
        config_.get_param<double>("cointegration_window", 200.0));
    params_.kalman_process_noise = config_.get_param<double>("kalman_process_noise", 1e-5);
    params_.kalman_measurement_noise = config_.get_param<double>("kalman_measurement_noise", 1e-3);
    params_.max_position = config_.get_param<double>("max_position", 1.0);
    params_.position_size_mult = config_.get_param<double>("position_size_mult", 1.0);
    params_.leg1_symbol = config_.get_param<std::string>("leg1_symbol", std::string("BTCUSDT"));
    params_.leg2_symbol = config_.get_param<std::string>("leg2_symbol", std::string("ETHUSDT"));
    params_.exchange = config_.get_param<std::string>("exchange", std::string("binance"));
    params_.min_holding_period_us = static_cast<int64_t>(
        config_.get_param<double>("min_holding_period_us", 60000000.0));
    params_.max_holding_period_us = static_cast<int64_t>(
        config_.get_param<double>("max_holding_period_us", 3600000000.0));
    params_.max_spread_deviation = config_.get_param<double>("max_spread_deviation", 5.0);
    params_.min_correlation = config_.get_param<double>("min_correlation", 0.7);
    params_.max_half_life = config_.get_param<double>("max_half_life", 100.0);
    params_.use_kalman_hedge = config_.get_param<bool>("use_kalman_hedge", true);
    params_.use_half_life_sizing = config_.get_param<bool>("use_half_life_sizing", true);
    params_.require_cointegration = config_.get_param<bool>("require_cointegration", false);

    // Initialize Kalman filter
    kalman_.process_noise = params_.kalman_process_noise;
    kalman_.measurement_noise = params_.kalman_measurement_noise;
    kalman_.variance = params_.kalman_initial_variance;
    kalman_.beta = 1.0;  // Initial hedge ratio guess

    // Initialize rolling stats
    spread_stats_.max_size = params_.lookback_period;
    spread_history_.max_size = params_.cointegration_window;
    leg1_.prices.max_size = params_.correlation_window;
    leg2_.prices.max_size = params_.correlation_window;
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

bool StatisticalArbitrageStrategy::initialize() {
    if (!StrategyBase::initialize()) {
        return false;
    }

    leg1_.symbol = params_.leg1_symbol;
    leg2_.symbol = params_.leg2_symbol;

    return true;
}

void StatisticalArbitrageStrategy::on_stop() {
    // Close any open position
    if (position_.is_active) {
        close_position("Strategy stopped");
    }
}

void StatisticalArbitrageStrategy::on_reset() {
    // Reset all state
    leg1_.prices.clear();
    leg2_.prices.clear();
    spread_stats_.clear();
    spread_history_.clear();

    kalman_.beta = 1.0;
    kalman_.variance = params_.kalman_initial_variance;

    position_ = PairPosition{};
    current_correlation_ = 0.0;
    current_half_life_ = 0.0;
    is_cointegrated_ = false;

    pending_leg1_order_ = 0;
    pending_leg2_order_ = 0;
    leg1_order_filled_ = false;
    leg2_order_filled_ = false;
}

// ============================================================================
// Event Handlers
// ============================================================================

void StatisticalArbitrageStrategy::on_market_data(const MarketData& data) {
    if (!is_running()) {
        return;
    }

    // Update backtest timestamp
    if (is_backtest_mode_) {
        set_backtest_timestamp(data.local_timestamp);
    }

    // Update leg data
    update_leg_data(data);

    // Only proceed if both legs have data
    if (!leg1_.is_valid() || !leg2_.is_valid()) {
        return;
    }

    // Update Kalman filter
    if (params_.use_kalman_hedge) {
        update_kalman_filter();
    }

    // Calculate spread and add to stats
    double spread = calculate_spread();
    spread_stats_.add(spread);
    spread_history_.add(spread);

    // Store prices for correlation calculation
    leg1_.prices.add(leg1_.last_price);
    leg2_.prices.add(leg2_.last_price);

    // Update statistics periodically
    if (spread_stats_.size() >= params_.lookback_period) {
        current_correlation_ = calculate_correlation();
        current_half_life_ = estimate_half_life();

        if (params_.require_cointegration && spread_history_.size() >= params_.cointegration_window) {
            double adf_stat = test_cointegration();
            // Critical value at 5% level is approximately -2.86 for ADF test
            is_cointegrated_ = adf_stat < -2.86;
        } else {
            is_cointegrated_ = true;  // Don't require if not enabled
        }
    }

    // Check holding period limits
    check_holding_limits();

    // Generate signals
    generate_signals();
}

void StatisticalArbitrageStrategy::on_order_update(const OrderUpdate& update) {
    if (!is_running()) {
        return;
    }

    if (update.order_id == pending_leg1_order_) {
        if (update.status == OrderStatus::FILLED) {
            leg1_order_filled_ = true;
            position_.leg1_quantity = update.side == OrderSide::BUY ?
                update.filled_quantity : -update.filled_quantity;
        } else if (update.is_terminal() && update.status != OrderStatus::FILLED) {
            // Leg 1 failed
            pending_leg1_order_ = 0;
            leg1_order_filled_ = false;

            // Cancel leg 2 if not filled
            if (!leg2_order_filled_ && pending_leg2_order_ != 0) {
                Signal cancel;
                cancel.type = SignalType::CLOSE_SHORT;
                cancel.symbol = leg2_.symbol;
                cancel.reason = "Cancelling leg 2 after leg 1 failure";
                emit_signal(std::move(cancel));
            }
        }
    } else if (update.order_id == pending_leg2_order_) {
        if (update.status == OrderStatus::FILLED) {
            leg2_order_filled_ = true;
            position_.leg2_quantity = update.side == OrderSide::BUY ?
                update.filled_quantity : -update.filled_quantity;
        } else if (update.is_terminal() && update.status != OrderStatus::FILLED) {
            // Leg 2 failed
            pending_leg2_order_ = 0;
            leg2_order_filled_ = false;

            // Need to close leg 1 if filled
            if (leg1_order_filled_) {
                Signal close;
                close.type = position_.leg1_quantity > 0 ? SignalType::SELL : SignalType::BUY;
                close.symbol = leg1_.symbol;
                close.exchange = params_.exchange;
                close.target_quantity = std::abs(position_.leg1_quantity);
                close.reason = "Closing leg 1 after leg 2 failure";
                close.urgency = 1.0;
                emit_signal(std::move(close));
            }
        }
    }

    // Check if both legs filled - position is now active
    if (leg1_order_filled_ && leg2_order_filled_) {
        position_.is_active = true;
        pending_leg1_order_ = 0;
        pending_leg2_order_ = 0;
        leg1_order_filled_ = false;
        leg2_order_filled_ = false;
    }
}

void StatisticalArbitrageStrategy::on_trade(const Trade& trade) {
    if (!is_running()) {
        return;
    }

    update_position(trade);
}

// ============================================================================
// Configuration
// ============================================================================

void StatisticalArbitrageStrategy::set_pair(const std::string& leg1, const std::string& leg2) {
    params_.leg1_symbol = leg1;
    params_.leg2_symbol = leg2;
    leg1_.symbol = leg1;
    leg2_.symbol = leg2;

    // Reset statistics
    on_reset();
}

// ============================================================================
// Analytics
// ============================================================================

StatisticalArbitrageStrategy::PairStats StatisticalArbitrageStrategy::get_pair_stats() const {
    PairStats stats;
    stats.current_spread = calculate_spread();
    stats.spread_mean = spread_stats_.mean();
    stats.spread_std = spread_stats_.stddev();
    stats.current_zscore = calculate_zscore();
    stats.correlation = current_correlation_;
    stats.hedge_ratio = kalman_.beta;
    stats.half_life = current_half_life_;
    stats.is_cointegrated = is_cointegrated_;
    stats.samples_collected = static_cast<int64_t>(spread_stats_.size());
    return stats;
}

double StatisticalArbitrageStrategy::get_current_zscore() const {
    return calculate_zscore();
}

double StatisticalArbitrageStrategy::get_hedge_ratio() const {
    return kalman_.beta;
}

// ============================================================================
// Core Logic Implementation
// ============================================================================

void StatisticalArbitrageStrategy::update_leg_data(const MarketData& data) {
    if (data.symbol == leg1_.symbol) {
        leg1_.last_price = data.last_price > 0 ? data.last_price : data.mid_price();
        leg1_.bid_price = data.bid_price;
        leg1_.ask_price = data.ask_price;
        leg1_.timestamp = data.local_timestamp;
    } else if (data.symbol == leg2_.symbol) {
        leg2_.last_price = data.last_price > 0 ? data.last_price : data.mid_price();
        leg2_.bid_price = data.bid_price;
        leg2_.ask_price = data.ask_price;
        leg2_.timestamp = data.local_timestamp;
    }
}

double StatisticalArbitrageStrategy::calculate_spread() const {
    if (!leg1_.is_valid() || !leg2_.is_valid()) {
        return 0.0;
    }

    // Spread = leg1 - beta * leg2
    return leg1_.last_price - kalman_.beta * leg2_.last_price;
}

double StatisticalArbitrageStrategy::calculate_zscore() const {
    if (spread_stats_.size() < 2) {
        return 0.0;
    }

    double spread = calculate_spread();
    double mean = spread_stats_.mean();
    double std = spread_stats_.stddev();

    if (std < 1e-10) {
        return 0.0;
    }

    return (spread - mean) / std;
}

void StatisticalArbitrageStrategy::update_kalman_filter() {
    if (!leg1_.is_valid() || !leg2_.is_valid()) {
        return;
    }

    kalman_.predict();
    kalman_.update(leg1_.last_price, leg2_.last_price);
}

double StatisticalArbitrageStrategy::calculate_correlation() const {
    if (leg1_.prices.size() < 3 || leg2_.prices.size() < 3) {
        return 0.0;
    }

    size_t n = std::min(leg1_.prices.size(), leg2_.prices.size());

    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0;
    double sum_x2 = 0.0, sum_y2 = 0.0;

    auto it1 = leg1_.prices.values.rbegin();
    auto it2 = leg2_.prices.values.rbegin();

    for (size_t i = 0; i < n; ++i, ++it1, ++it2) {
        double x = *it1;
        double y = *it2;
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
        sum_y2 += y * y;
    }

    double dn = static_cast<double>(n);
    double numerator = dn * sum_xy - sum_x * sum_y;
    double denominator = std::sqrt((dn * sum_x2 - sum_x * sum_x) *
                                   (dn * sum_y2 - sum_y * sum_y));

    if (denominator < 1e-10) {
        return 0.0;
    }

    return numerator / denominator;
}

double StatisticalArbitrageStrategy::estimate_half_life() const {
    // Estimate half-life using OLS regression on spread changes
    // dS(t) = lambda * S(t-1) + epsilon
    // half_life = -ln(2) / lambda

    if (spread_history_.size() < 10) {
        return std::numeric_limits<double>::infinity();
    }

    const auto& spreads = spread_history_.values;
    size_t n = spreads.size() - 1;

    // Calculate means
    double sum_x = 0.0, sum_y = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum_x += spreads[i];
        sum_y += spreads[i + 1] - spreads[i];
    }
    double mean_x = sum_x / n;
    double mean_y = sum_y / n;

    // Calculate beta (lambda) using OLS
    double numerator = 0.0, denominator = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double x_dev = spreads[i] - mean_x;
        double y_dev = (spreads[i + 1] - spreads[i]) - mean_y;
        numerator += x_dev * y_dev;
        denominator += x_dev * x_dev;
    }

    if (denominator < 1e-10) {
        return std::numeric_limits<double>::infinity();
    }

    double lambda = numerator / denominator;

    // lambda should be negative for mean reversion
    if (lambda >= 0) {
        return std::numeric_limits<double>::infinity();
    }

    return -std::log(2.0) / lambda;
}

double StatisticalArbitrageStrategy::test_cointegration() const {
    // Simplified ADF test on spread residuals
    // Returns t-statistic

    if (spread_history_.size() < 20) {
        return 0.0;  // Not enough data
    }

    const auto& spreads = spread_history_.values;
    size_t n = spreads.size() - 1;

    // Run regression: dS(t) = alpha + lambda * S(t-1) + epsilon
    double sum_x = 0.0, sum_y = 0.0;
    double sum_x2 = 0.0, sum_xy = 0.0;

    for (size_t i = 0; i < n; ++i) {
        double x = spreads[i];
        double y = spreads[i + 1] - spreads[i];
        sum_x += x;
        sum_y += y;
        sum_x2 += x * x;
        sum_xy += x * y;
    }

    double dn = static_cast<double>(n);
    double det = dn * sum_x2 - sum_x * sum_x;

    if (std::abs(det) < 1e-10) {
        return 0.0;
    }

    // Solve for lambda
    double lambda = (dn * sum_xy - sum_x * sum_y) / det;
    double alpha = (sum_y - lambda * sum_x) / dn;

    // Calculate residual variance
    double sse = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double y = spreads[i + 1] - spreads[i];
        double y_hat = alpha + lambda * spreads[i];
        double residual = y - y_hat;
        sse += residual * residual;
    }

    double mse = sse / (dn - 2);
    double var_lambda = mse * dn / det;
    double se_lambda = std::sqrt(var_lambda);

    if (se_lambda < 1e-10) {
        return 0.0;
    }

    // t-statistic
    return lambda / se_lambda;
}

void StatisticalArbitrageStrategy::generate_signals() {
    // Need enough data
    if (spread_stats_.size() < params_.lookback_period / 2) {
        return;
    }

    // Check correlation requirement
    if (current_correlation_ < params_.min_correlation) {
        return;
    }

    // Check cointegration requirement
    if (params_.require_cointegration && !is_cointegrated_) {
        return;
    }

    // Check half-life requirement
    if (current_half_life_ > params_.max_half_life ||
        current_half_life_ <= 0 ||
        std::isinf(current_half_life_)) {
        return;
    }

    double zscore = calculate_zscore();

    // Check for stop loss
    if (position_.is_active && std::abs(zscore) > params_.stop_zscore) {
        close_position("Stop loss triggered (z-score: " + std::to_string(zscore) + ")");
        return;
    }

    // Check for exit
    if (position_.is_active) {
        if (should_close_position()) {
            close_position("Mean reversion target reached");
            return;
        }
    }

    // Check for entry
    if (!position_.is_active && pending_leg1_order_ == 0) {
        // Rate limit signals
        auto now = current_timestamp();
        if ((now - last_signal_time_).count() < MIN_SIGNAL_INTERVAL_NS) {
            return;
        }

        // Check risk limits
        double notional = params_.max_position * leg1_.last_price;
        if (!can_open_position(notional)) {
            return;
        }

        if (zscore > params_.entry_zscore) {
            // Spread is above mean - expect it to decrease
            // Short spread: sell leg1, buy leg2
            open_position(false);
            last_signal_time_ = now;
        } else if (zscore < -params_.entry_zscore) {
            // Spread is below mean - expect it to increase
            // Long spread: buy leg1, sell leg2
            open_position(true);
            last_signal_time_ = now;
        }
    }
}

double StatisticalArbitrageStrategy::calculate_position_size() const {
    double base_size = params_.max_position * params_.position_size_mult;

    if (params_.use_half_life_sizing && current_half_life_ > 0 &&
        !std::isinf(current_half_life_)) {
        // Reduce size for longer half-lives
        double hl_factor = std::min(1.0, params_.max_half_life / (2.0 * current_half_life_));
        base_size *= hl_factor;
    }

    // Adjust based on z-score magnitude (higher z-score = higher conviction)
    double zscore = std::abs(calculate_zscore());
    if (zscore > params_.entry_zscore) {
        double zscore_factor = std::min(1.5, 1.0 + (zscore - params_.entry_zscore) / params_.entry_zscore);
        base_size *= zscore_factor;
    }

    return std::min(base_size, params_.max_position);
}

bool StatisticalArbitrageStrategy::should_close_position() const {
    if (!position_.is_active) {
        return false;
    }

    double zscore = calculate_zscore();
    bool was_long = position_.entry_zscore < 0;

    // Exit when z-score crosses zero and is below exit threshold
    if (was_long) {
        // Was long spread (entry z < 0), exit when z > 0 and < exit_zscore
        return zscore >= 0 && zscore < params_.exit_zscore;
    } else {
        // Was short spread (entry z > 0), exit when z < 0 and > -exit_zscore
        return zscore <= 0 && zscore > -params_.exit_zscore;
    }
}

void StatisticalArbitrageStrategy::open_position(bool long_spread) {
    double size = calculate_position_size();
    double hedge_qty = size * kalman_.beta;

    // Store entry information
    position_.entry_zscore = calculate_zscore();
    position_.entry_spread = calculate_spread();
    position_.entry_time = current_timestamp();

    if (long_spread) {
        // Long spread: buy leg1, sell leg2
        Signal buy_leg1;
        buy_leg1.type = SignalType::BUY;
        buy_leg1.symbol = leg1_.symbol;
        buy_leg1.exchange = params_.exchange;
        buy_leg1.target_price = leg1_.ask_price;
        buy_leg1.target_quantity = size;
        buy_leg1.confidence = std::abs(position_.entry_zscore) / params_.stop_zscore;
        buy_leg1.reason = "Stat arb: long spread, z=" + std::to_string(position_.entry_zscore);
        buy_leg1.timeout_us = config_.order_timeout_us;

        Signal sell_leg2;
        sell_leg2.type = SignalType::SELL;
        sell_leg2.symbol = leg2_.symbol;
        sell_leg2.exchange = params_.exchange;
        sell_leg2.target_price = leg2_.bid_price;
        sell_leg2.target_quantity = hedge_qty;
        sell_leg2.confidence = buy_leg1.confidence;
        sell_leg2.reason = buy_leg1.reason;
        sell_leg2.timeout_us = config_.order_timeout_us;

        pending_leg1_order_ = current_timestamp().count();
        pending_leg2_order_ = pending_leg1_order_ + 1;

        emit_signal(std::move(buy_leg1));
        emit_signal(std::move(sell_leg2));
    } else {
        // Short spread: sell leg1, buy leg2
        Signal sell_leg1;
        sell_leg1.type = SignalType::SELL;
        sell_leg1.symbol = leg1_.symbol;
        sell_leg1.exchange = params_.exchange;
        sell_leg1.target_price = leg1_.bid_price;
        sell_leg1.target_quantity = size;
        sell_leg1.confidence = std::abs(position_.entry_zscore) / params_.stop_zscore;
        sell_leg1.reason = "Stat arb: short spread, z=" + std::to_string(position_.entry_zscore);
        sell_leg1.timeout_us = config_.order_timeout_us;

        Signal buy_leg2;
        buy_leg2.type = SignalType::BUY;
        buy_leg2.symbol = leg2_.symbol;
        buy_leg2.exchange = params_.exchange;
        buy_leg2.target_price = leg2_.ask_price;
        buy_leg2.target_quantity = hedge_qty;
        buy_leg2.confidence = sell_leg1.confidence;
        buy_leg2.reason = sell_leg1.reason;
        buy_leg2.timeout_us = config_.order_timeout_us;

        pending_leg1_order_ = current_timestamp().count();
        pending_leg2_order_ = pending_leg1_order_ + 1;

        emit_signal(std::move(sell_leg1));
        emit_signal(std::move(buy_leg2));
    }
}

void StatisticalArbitrageStrategy::close_position(const std::string& reason) {
    if (!position_.is_active) {
        return;
    }

    // Close leg1
    Signal close_leg1;
    close_leg1.type = position_.leg1_quantity > 0 ? SignalType::SELL : SignalType::BUY;
    close_leg1.symbol = leg1_.symbol;
    close_leg1.exchange = params_.exchange;
    close_leg1.target_price = position_.leg1_quantity > 0 ? leg1_.bid_price : leg1_.ask_price;
    close_leg1.target_quantity = std::abs(position_.leg1_quantity);
    close_leg1.reason = reason;
    close_leg1.urgency = 0.8;
    close_leg1.timeout_us = config_.order_timeout_us;

    // Close leg2
    Signal close_leg2;
    close_leg2.type = position_.leg2_quantity > 0 ? SignalType::SELL : SignalType::BUY;
    close_leg2.symbol = leg2_.symbol;
    close_leg2.exchange = params_.exchange;
    close_leg2.target_price = position_.leg2_quantity > 0 ? leg2_.bid_price : leg2_.ask_price;
    close_leg2.target_quantity = std::abs(position_.leg2_quantity);
    close_leg2.reason = reason;
    close_leg2.urgency = 0.8;
    close_leg2.timeout_us = config_.order_timeout_us;

    emit_signal(std::move(close_leg1));
    emit_signal(std::move(close_leg2));

    // Reset position
    position_ = PairPosition{};
}

void StatisticalArbitrageStrategy::check_holding_limits() {
    if (!position_.is_active) {
        return;
    }

    auto now = current_timestamp();
    int64_t holding_time_us = (now - position_.entry_time).count() / 1000;

    if (holding_time_us > params_.max_holding_period_us) {
        close_position("Maximum holding period exceeded");
    }
}

}  // namespace strategies
}  // namespace hft
