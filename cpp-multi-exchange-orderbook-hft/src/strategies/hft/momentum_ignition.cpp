#include "strategies/hft/momentum_ignition.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace hft {
namespace strategies {

// ============================================================================
// Constructor
// ============================================================================

MomentumIgnitionStrategy::MomentumIgnitionStrategy(StrategyConfig config)
    : StrategyBase(std::move(config))
{
    // Load parameters from config
    params_.momentum_threshold = config_.get_param<double>("momentum_threshold", 0.001);
    params_.acceleration_threshold = config_.get_param<double>("acceleration_threshold", 0.5);
    params_.momentum_window = static_cast<size_t>(
        config_.get_param<double>("momentum_window", 20.0));
    params_.acceleration_window = static_cast<size_t>(
        config_.get_param<double>("acceleration_window", 5.0));
    params_.volume_surge_threshold = config_.get_param<double>("volume_surge_threshold", 2.0);
    params_.volume_window = static_cast<size_t>(
        config_.get_param<double>("volume_window", 50.0));
    params_.min_volume_ratio = config_.get_param<double>("min_volume_ratio", 1.5);
    params_.imbalance_threshold = config_.get_param<double>("imbalance_threshold", 0.6);
    params_.orderbook_levels = static_cast<size_t>(
        config_.get_param<double>("orderbook_levels", 5.0));
    params_.max_position = config_.get_param<double>("max_position", 1.0);
    params_.position_size_pct = config_.get_param<double>("position_size_pct", 0.1);
    params_.max_hold_time_us = static_cast<int64_t>(
        config_.get_param<double>("max_hold_time_us", 30000000.0));
    params_.min_hold_time_us = static_cast<int64_t>(
        config_.get_param<double>("min_hold_time_us", 1000000.0));
    params_.stop_loss_pct = config_.get_param<double>("stop_loss_pct", 0.002);
    params_.take_profit_pct = config_.get_param<double>("take_profit_pct", 0.003);
    params_.trailing_stop_pct = config_.get_param<double>("trailing_stop_pct", 0.001);
    params_.min_confidence = config_.get_param<double>("min_confidence", 0.6);
    params_.min_samples = static_cast<size_t>(
        config_.get_param<double>("min_samples", 30.0));
    params_.exchange = config_.get_param<std::string>("exchange", std::string("binance"));
    params_.use_adaptive_thresholds = config_.get_param<bool>("use_adaptive_thresholds", true);
    params_.volatility_window = config_.get_param<double>("volatility_window", 100.0);
    params_.volatility_scale = config_.get_param<double>("volatility_scale", 1.5);
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

bool MomentumIgnitionStrategy::initialize() {
    if (!StrategyBase::initialize()) {
        return false;
    }

    // Initialize state for each configured symbol
    for (const auto& symbol : config_.symbols) {
        states_[symbol].symbol = symbol;
    }

    return true;
}

void MomentumIgnitionStrategy::on_stop() {
    // Close all positions
    for (auto& [symbol, state] : states_) {
        if (state.has_position()) {
            execute_exit(state, "Strategy stopped");
        }
    }
}

void MomentumIgnitionStrategy::on_reset() {
    states_.clear();
    order_to_symbol_.clear();
    last_signal_time_.clear();

    for (const auto& symbol : config_.symbols) {
        states_[symbol].symbol = symbol;
    }
}

// ============================================================================
// Event Handlers
// ============================================================================

void MomentumIgnitionStrategy::on_market_data(const MarketData& data) {
    if (!is_running()) {
        return;
    }

    // Update backtest timestamp
    if (is_backtest_mode_) {
        set_backtest_timestamp(data.local_timestamp);
    }

    // Get or create state
    auto& state = states_[data.symbol];
    if (state.symbol.empty()) {
        state.symbol = data.symbol;
    }

    // Update state
    update_state(state, data);
    update_bars(state, data);

    // Calculate metrics
    state.current_volatility = calculate_volatility(state);

    // Add imbalance to history
    double imbalance = calculate_imbalance(data);
    state.imbalances.push_back(imbalance);
    while (state.imbalances.size() > params_.momentum_window) {
        state.imbalances.pop_front();
    }

    // Check exit conditions for existing position
    if (state.has_position()) {
        check_exit_conditions(state);

        // Check holding time
        auto now = current_timestamp();
        int64_t hold_time = (now - state.entry_time).count() / 1000;
        if (hold_time > params_.max_hold_time_us) {
            execute_exit(state, "Max holding time exceeded");
            return;
        }
    }

    // Need enough samples before trading
    if (state.returns.size() < params_.min_samples) {
        return;
    }

    // Detect momentum signal
    MomentumSignal signal = detect_momentum_signal(state);

    // Check entry conditions
    if (!state.has_position() && should_enter(state, signal)) {
        execute_entry(state, signal);
    }
}

void MomentumIgnitionStrategy::on_order_update(const OrderUpdate& update) {
    if (!is_running()) {
        return;
    }

    auto it = order_to_symbol_.find(update.order_id);
    if (it == order_to_symbol_.end()) {
        return;
    }

    const std::string& symbol = it->second;
    auto state_it = states_.find(symbol);
    if (state_it == states_.end()) {
        return;
    }

    auto& state = state_it->second;

    if (update.status == OrderStatus::FILLED) {
        // Update position
        double fill_qty = update.filled_quantity;
        if (update.side == OrderSide::SELL) {
            fill_qty = -fill_qty;
        }

        if (std::abs(state.position_qty) < 1e-10) {
            // New position
            state.position_qty = fill_qty;
            state.entry_price = update.average_fill_price;
            state.highest_since_entry = update.average_fill_price;
            state.lowest_since_entry = update.average_fill_price;
            state.entry_time = current_timestamp();
        } else if ((state.position_qty > 0 && fill_qty < 0) ||
                   (state.position_qty < 0 && fill_qty > 0)) {
            // Closing position
            double pnl = std::abs(state.position_qty) *
                        (update.average_fill_price - state.entry_price) *
                        (state.position_qty > 0 ? 1 : -1);
            state.total_pnl += pnl;
            state.total_trades++;
            if (pnl > 0) {
                state.winning_trades++;
            }
            state.position_qty = 0;
            state.entry_price = 0;
        } else {
            // Adding to position
            double old_notional = std::abs(state.position_qty) * state.entry_price;
            double new_notional = std::abs(fill_qty) * update.average_fill_price;
            state.position_qty += fill_qty;
            state.entry_price = (old_notional + new_notional) / std::abs(state.position_qty);
        }
    }

    if (update.is_terminal()) {
        order_to_symbol_.erase(it);
    }
}

void MomentumIgnitionStrategy::on_trade(const Trade& trade) {
    if (!is_running()) {
        return;
    }

    update_position(trade);
}

// ============================================================================
// Analytics
// ============================================================================

MomentumIgnitionStrategy::MomentumStats
MomentumIgnitionStrategy::get_momentum_stats(const std::string& symbol) const {
    MomentumStats stats;

    auto it = states_.find(symbol);
    if (it == states_.end()) {
        return stats;
    }

    const auto& state = it->second;
    stats.current_momentum = calculate_momentum(state);
    stats.momentum_acceleration = calculate_acceleration(state);
    stats.volume_ratio = calculate_volume_ratio(state);
    stats.orderbook_imbalance = state.imbalances.empty() ? 0.0 : state.imbalances.back();
    stats.volatility = state.current_volatility;
    stats.vwap = state.session_vwap;
    stats.trades_today = state.total_trades;
    stats.winning_trades = state.winning_trades;
    stats.total_pnl = state.total_pnl;

    return stats;
}

double MomentumIgnitionStrategy::get_current_momentum(const std::string& symbol) const {
    auto it = states_.find(symbol);
    if (it == states_.end()) {
        return 0.0;
    }
    return calculate_momentum(it->second);
}

// ============================================================================
// Core Logic Implementation
// ============================================================================

void MomentumIgnitionStrategy::update_state(SymbolState& state, const MarketData& data) {
    state.last_price = data.last_price > 0 ? data.last_price : data.mid_price();
    state.bid_price = data.bid_price;
    state.ask_price = data.ask_price;
    state.bid_size = data.bid_size;
    state.ask_size = data.ask_size;
    state.exchange_id = data.exchange_id;

    // Update session VWAP
    if (data.last_size > 0 && data.last_price > 0) {
        double new_volume = state.session_volume + data.last_size;
        state.session_vwap = (state.session_vwap * state.session_volume +
                              data.last_price * data.last_size) / new_volume;
        state.session_volume = new_volume;
    }

    // Update price extremes for position
    if (state.has_position()) {
        state.highest_since_entry = std::max(state.highest_since_entry, state.last_price);
        state.lowest_since_entry = std::min(state.lowest_since_entry, state.last_price);
    }
}

void MomentumIgnitionStrategy::update_bars(SymbolState& state, const MarketData& data) {
    auto now = data.local_timestamp;

    // Initialize first bar
    if (state.current_bar.trade_count == 0) {
        state.current_bar.reset(state.last_price, now);
    }

    // Check if we need to close current bar
    if ((now - state.current_bar.timestamp).count() >= state.bar_interval_ns) {
        // Save bar
        state.bars.push_back(state.current_bar);
        while (state.bars.size() > params_.volume_window * 2) {
            state.bars.pop_front();
        }

        // Calculate return
        if (state.bars.size() >= 2) {
            auto& prev = state.bars[state.bars.size() - 2];
            auto& curr = state.bars.back();
            if (prev.close > 0) {
                double ret = (curr.close - prev.close) / prev.close;
                state.returns.push_back(ret);
                while (state.returns.size() > params_.volatility_window) {
                    state.returns.pop_front();
                }
            }

            // Store volume
            state.volumes.push_back(curr.volume);
            while (state.volumes.size() > params_.volume_window) {
                state.volumes.pop_front();
            }
        }

        // Calculate momentum
        double momentum = calculate_momentum(state);
        state.momenta.push_back(momentum);
        while (state.momenta.size() > params_.acceleration_window * 2) {
            state.momenta.pop_front();
        }

        // Start new bar
        state.current_bar.reset(state.last_price, now);
    }

    // Update current bar
    if (data.last_size > 0) {
        state.current_bar.update(state.last_price, data.last_size);
    }
}

double MomentumIgnitionStrategy::calculate_momentum(const SymbolState& state) const {
    if (state.returns.size() < params_.momentum_window) {
        return 0.0;
    }

    // Calculate momentum as sum of recent returns
    double momentum = 0.0;
    size_t count = 0;
    for (auto it = state.returns.rbegin();
         it != state.returns.rend() && count < params_.momentum_window;
         ++it, ++count) {
        momentum += *it;
    }

    return momentum;
}

double MomentumIgnitionStrategy::calculate_acceleration(const SymbolState& state) const {
    if (state.momenta.size() < params_.acceleration_window) {
        return 0.0;
    }

    // Calculate momentum change over recent period
    double recent_momentum = 0.0;
    double older_momentum = 0.0;
    size_t half = params_.acceleration_window / 2;
    size_t count = 0;

    for (auto it = state.momenta.rbegin();
         it != state.momenta.rend() && count < params_.acceleration_window;
         ++it, ++count) {
        if (count < half) {
            recent_momentum += *it;
        } else {
            older_momentum += *it;
        }
    }

    if (half > 0) {
        recent_momentum /= half;
        older_momentum /= (params_.acceleration_window - half);
    }

    return recent_momentum - older_momentum;
}

double MomentumIgnitionStrategy::calculate_volume_ratio(const SymbolState& state) const {
    if (state.volumes.empty()) {
        return 1.0;
    }

    // Average volume
    double avg_volume = 0.0;
    for (double v : state.volumes) {
        avg_volume += v;
    }
    avg_volume /= state.volumes.size();

    if (avg_volume < 1e-10) {
        return 1.0;
    }

    // Current bar volume
    double current_volume = state.current_bar.volume;
    if (state.volumes.size() > 0) {
        current_volume = state.volumes.back();
    }

    return current_volume / avg_volume;
}

double MomentumIgnitionStrategy::calculate_imbalance(const MarketData& data) const {
    // Simple bid/ask imbalance at top of book
    double total = data.bid_size + data.ask_size;
    if (total < 1e-10) {
        return 0.0;
    }

    // Positive = more bids (bullish), Negative = more asks (bearish)
    return (data.bid_size - data.ask_size) / total;
}

double MomentumIgnitionStrategy::calculate_volatility(const SymbolState& state) const {
    if (state.returns.size() < 10) {
        return 0.01;  // Default volatility
    }

    // Calculate standard deviation of returns
    double sum = 0.0;
    double sum_sq = 0.0;
    for (double r : state.returns) {
        sum += r;
        sum_sq += r * r;
    }

    double n = static_cast<double>(state.returns.size());
    double variance = (sum_sq - sum * sum / n) / (n - 1);

    return std::sqrt(std::max(0.0, variance));
}

MomentumIgnitionStrategy::MomentumSignal
MomentumIgnitionStrategy::detect_momentum_signal(const SymbolState& state) const {
    MomentumSignal signal;

    double momentum = calculate_momentum(state);
    double acceleration = calculate_acceleration(state);
    double volume_ratio = calculate_volume_ratio(state);

    // Get current imbalance
    double imbalance = state.imbalances.empty() ? 0.0 : state.imbalances.back();

    // Adaptive thresholds
    double mom_threshold = params_.momentum_threshold;
    double acc_threshold = params_.acceleration_threshold;
    if (params_.use_adaptive_thresholds && state.current_volatility > 0) {
        double vol_factor = state.current_volatility * params_.volatility_scale;
        mom_threshold *= std::max(0.5, std::min(2.0, vol_factor / 0.01));
    }

    signal.momentum = momentum;
    signal.acceleration = acceleration;
    signal.volume_ratio = volume_ratio;
    signal.imbalance = imbalance;

    // Detect bullish momentum ignition
    bool bullish_momentum = momentum > mom_threshold;
    bool bullish_acceleration = acceleration > acc_threshold * mom_threshold;
    bool bullish_volume = volume_ratio > params_.min_volume_ratio;
    bool bullish_imbalance = imbalance > params_.imbalance_threshold;

    // Detect bearish momentum ignition
    bool bearish_momentum = momentum < -mom_threshold;
    bool bearish_acceleration = acceleration < -acc_threshold * mom_threshold;
    bool bearish_volume = volume_ratio > params_.min_volume_ratio;
    bool bearish_imbalance = imbalance < -params_.imbalance_threshold;

    // Score bullish signals
    int bullish_score = 0;
    if (bullish_momentum) bullish_score += 2;
    if (bullish_acceleration) bullish_score += 2;
    if (bullish_volume) bullish_score += 1;
    if (bullish_imbalance) bullish_score += 1;

    // Score bearish signals
    int bearish_score = 0;
    if (bearish_momentum) bearish_score += 2;
    if (bearish_acceleration) bearish_score += 2;
    if (bearish_volume) bearish_score += 1;
    if (bearish_imbalance) bearish_score += 1;

    // Generate signal
    if (bullish_score >= 4 && bullish_score > bearish_score) {
        signal.type = SignalType::BUY;
        signal.confidence = calculate_confidence(signal, state);
        signal.reason = "Bullish momentum ignition: mom=" +
                       std::to_string(momentum) +
                       " acc=" + std::to_string(acceleration) +
                       " vol=" + std::to_string(volume_ratio);
    } else if (bearish_score >= 4 && bearish_score > bullish_score) {
        signal.type = SignalType::SELL;
        signal.confidence = calculate_confidence(signal, state);
        signal.reason = "Bearish momentum ignition: mom=" +
                       std::to_string(momentum) +
                       " acc=" + std::to_string(acceleration) +
                       " vol=" + std::to_string(volume_ratio);
    }

    return signal;
}

double MomentumIgnitionStrategy::calculate_confidence(
    const MomentumSignal& signal, const SymbolState& state) const {

    double confidence = 0.5;  // Base confidence

    // Adjust based on momentum strength
    double mom_strength = std::abs(signal.momentum) / params_.momentum_threshold;
    confidence += std::min(0.2, mom_strength * 0.1);

    // Adjust based on acceleration
    double acc_strength = std::abs(signal.acceleration) /
                         (params_.acceleration_threshold * params_.momentum_threshold);
    confidence += std::min(0.15, acc_strength * 0.1);

    // Adjust based on volume
    if (signal.volume_ratio > params_.volume_surge_threshold) {
        confidence += 0.1;
    } else if (signal.volume_ratio > params_.min_volume_ratio) {
        confidence += 0.05;
    }

    // Adjust based on imbalance alignment
    bool aligned = (signal.type == SignalType::BUY && signal.imbalance > 0) ||
                   (signal.type == SignalType::SELL && signal.imbalance < 0);
    if (aligned) {
        confidence += std::abs(signal.imbalance) * 0.1;
    }

    // Reduce confidence in high volatility
    if (state.current_volatility > 0.02) {
        confidence *= 0.8;
    }

    return std::clamp(confidence, 0.0, 1.0);
}

bool MomentumIgnitionStrategy::should_enter(
    const SymbolState& state, const MomentumSignal& signal) const {

    if (signal.type == SignalType::NONE) {
        return false;
    }

    if (signal.confidence < params_.min_confidence) {
        return false;
    }

    // Check signal interval
    auto it = last_signal_time_.find(state.symbol);
    if (it != last_signal_time_.end()) {
        auto now = current_timestamp();
        if ((now - it->second).count() < MIN_SIGNAL_INTERVAL_NS) {
            return false;
        }
    }

    // Check risk limits
    double notional = calculate_size(state, signal) * state.last_price;
    if (!can_open_position(notional)) {
        return false;
    }

    return true;
}

bool MomentumIgnitionStrategy::should_exit(const SymbolState& state) const {
    if (!state.has_position()) {
        return false;
    }

    // Check minimum hold time
    auto now = current_timestamp();
    int64_t hold_time = (now - state.entry_time).count() / 1000;
    if (hold_time < params_.min_hold_time_us) {
        return false;
    }

    // Check momentum reversal
    double momentum = calculate_momentum(state);
    if (state.is_long() && momentum < -params_.momentum_threshold / 2) {
        return true;  // Momentum reversed for long position
    }
    if (state.is_short() && momentum > params_.momentum_threshold / 2) {
        return true;  // Momentum reversed for short position
    }

    return false;
}

double MomentumIgnitionStrategy::calculate_size(
    const SymbolState& state, const MomentumSignal& signal) const {

    double base_size = params_.max_position * params_.position_size_pct;

    // Scale by confidence
    base_size *= signal.confidence;

    // Scale by momentum strength (capped)
    double mom_factor = std::min(2.0, std::abs(signal.momentum) / params_.momentum_threshold);
    base_size *= (0.5 + 0.5 * mom_factor);

    // Cap at max position
    return std::min(base_size, params_.max_position);
}

void MomentumIgnitionStrategy::execute_entry(
    SymbolState& state, const MomentumSignal& signal) {

    double size = calculate_size(state, signal);

    Signal order_signal;
    order_signal.type = signal.type;
    order_signal.symbol = state.symbol;
    order_signal.exchange = params_.exchange;
    order_signal.exchange_id = state.exchange_id;
    order_signal.target_quantity = size;
    order_signal.confidence = signal.confidence;
    order_signal.reason = signal.reason;
    order_signal.urgency = 0.9;  // High urgency for momentum

    if (signal.type == SignalType::BUY) {
        order_signal.target_price = state.ask_price * 1.001;  // Small buffer
        order_signal.stop_loss = state.last_price * (1.0 - params_.stop_loss_pct);
        order_signal.take_profit = state.last_price * (1.0 + params_.take_profit_pct);
    } else {
        order_signal.target_price = state.bid_price * 0.999;
        order_signal.stop_loss = state.last_price * (1.0 + params_.stop_loss_pct);
        order_signal.take_profit = state.last_price * (1.0 - params_.take_profit_pct);
    }

    order_signal.timeout_us = params_.max_hold_time_us;

    // Track the order
    uint64_t order_id = current_timestamp().count();
    order_to_symbol_[order_id] = state.symbol;
    last_signal_time_[state.symbol] = current_timestamp();

    emit_signal(std::move(order_signal));
}

void MomentumIgnitionStrategy::execute_exit(
    SymbolState& state, const std::string& reason) {

    if (!state.has_position()) {
        return;
    }

    Signal order_signal;
    order_signal.type = state.is_long() ? SignalType::SELL : SignalType::BUY;
    order_signal.symbol = state.symbol;
    order_signal.exchange = params_.exchange;
    order_signal.exchange_id = state.exchange_id;
    order_signal.target_quantity = std::abs(state.position_qty);
    order_signal.target_price = state.is_long() ? state.bid_price : state.ask_price;
    order_signal.reason = reason;
    order_signal.urgency = 1.0;  // Maximum urgency for exit
    order_signal.confidence = 1.0;

    uint64_t order_id = current_timestamp().count();
    order_to_symbol_[order_id] = state.symbol;

    emit_signal(std::move(order_signal));
}

void MomentumIgnitionStrategy::check_exit_conditions(SymbolState& state) {
    if (!state.has_position()) {
        return;
    }

    double current_pnl_pct;
    if (state.is_long()) {
        current_pnl_pct = (state.last_price - state.entry_price) / state.entry_price;
    } else {
        current_pnl_pct = (state.entry_price - state.last_price) / state.entry_price;
    }

    // Check stop loss
    if (current_pnl_pct < -params_.stop_loss_pct) {
        execute_exit(state, "Stop loss hit: " + std::to_string(current_pnl_pct * 100) + "%");
        return;
    }

    // Check take profit
    if (current_pnl_pct > params_.take_profit_pct) {
        execute_exit(state, "Take profit hit: " + std::to_string(current_pnl_pct * 100) + "%");
        return;
    }

    // Check trailing stop
    double trailing_pnl;
    if (state.is_long()) {
        trailing_pnl = (state.last_price - state.highest_since_entry) / state.highest_since_entry;
    } else {
        trailing_pnl = (state.lowest_since_entry - state.last_price) / state.lowest_since_entry;
    }

    if (current_pnl_pct > params_.trailing_stop_pct &&
        trailing_pnl < -params_.trailing_stop_pct) {
        execute_exit(state, "Trailing stop hit");
        return;
    }

    // Check momentum reversal
    if (should_exit(state)) {
        execute_exit(state, "Momentum reversal detected");
        return;
    }
}

double MomentumIgnitionStrategy::get_adaptive_threshold(
    double base_threshold, double volatility) const {

    if (!params_.use_adaptive_thresholds || volatility <= 0) {
        return base_threshold;
    }

    // Scale threshold with volatility
    double vol_factor = volatility * params_.volatility_scale / 0.01;  // Normalize to 1% vol
    return base_threshold * std::max(0.5, std::min(2.0, vol_factor));
}

}  // namespace strategies
}  // namespace hft
