#include "strategies/hft/order_flow_imbalance.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace hft {
namespace strategies {

// ============================================================================
// Constructor
// ============================================================================

OrderFlowImbalanceStrategy::OrderFlowImbalanceStrategy(StrategyConfig config)
    : StrategyBase(std::move(config))
{
    // Load parameters from config
    params_.orderbook_levels = static_cast<size_t>(
        config_.get_param<double>("orderbook_levels", 10.0));
    params_.level_decay = config_.get_param<double>("level_decay", 0.8);
    params_.imbalance_threshold = config_.get_param<double>("imbalance_threshold", 0.3);
    params_.vpin_buckets = static_cast<size_t>(
        config_.get_param<double>("vpin_buckets", 50.0));
    params_.bucket_size = config_.get_param<double>("bucket_size", 1000.0);
    params_.vpin_threshold = config_.get_param<double>("vpin_threshold", 0.7);
    params_.trade_window = static_cast<size_t>(
        config_.get_param<double>("trade_window", 100.0));
    params_.trade_imbalance_threshold = config_.get_param<double>("trade_imbalance_threshold", 0.6);
    params_.aggressor_threshold = config_.get_param<double>("aggressor_threshold", 0.65);
    params_.pressure_window = static_cast<size_t>(
        config_.get_param<double>("pressure_window", 20.0));
    params_.pressure_threshold = config_.get_param<double>("pressure_threshold", 0.4);
    params_.min_confidence = config_.get_param<double>("min_confidence", 0.6);
    params_.signal_decay = config_.get_param<double>("signal_decay", 0.95);
    params_.signal_lookback = static_cast<size_t>(
        config_.get_param<double>("signal_lookback", 10.0));
    params_.max_position = config_.get_param<double>("max_position", 1.0);
    params_.position_size_pct = config_.get_param<double>("position_size_pct", 0.05);
    params_.max_hold_time_us = static_cast<int64_t>(
        config_.get_param<double>("max_hold_time_us", 60000000.0));
    params_.min_signal_interval_us = static_cast<int64_t>(
        config_.get_param<double>("min_signal_interval_us", 500000.0));
    params_.stop_loss_pct = config_.get_param<double>("stop_loss_pct", 0.003);
    params_.take_profit_pct = config_.get_param<double>("take_profit_pct", 0.004);
    params_.max_spread_bps = config_.get_param<double>("max_spread_bps", 10.0);
    params_.exchange = config_.get_param<std::string>("exchange", std::string("binance"));
    params_.use_vpin = config_.get_param<bool>("use_vpin", true);
    params_.use_orderbook_pressure = config_.get_param<bool>("use_orderbook_pressure", true);
    params_.use_trade_flow = config_.get_param<bool>("use_trade_flow", true);
    params_.use_multi_level_imbalance = config_.get_param<bool>("use_multi_level_imbalance", true);
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

bool OrderFlowImbalanceStrategy::initialize() {
    if (!StrategyBase::initialize()) {
        return false;
    }

    // Initialize state for configured symbols
    for (const auto& symbol : config_.symbols) {
        states_[symbol].symbol = symbol;
        states_[symbol].bids.resize(params_.orderbook_levels);
        states_[symbol].asks.resize(params_.orderbook_levels);
        states_[symbol].prev_bids.resize(params_.orderbook_levels);
        states_[symbol].prev_asks.resize(params_.orderbook_levels);
    }

    return true;
}

void OrderFlowImbalanceStrategy::on_stop() {
    // Close all positions
    for (auto& [symbol, state] : states_) {
        if (state.has_position()) {
            execute_exit(state, "Strategy stopped");
        }
    }
}

void OrderFlowImbalanceStrategy::on_reset() {
    states_.clear();
    order_to_symbol_.clear();
    last_signal_time_.clear();

    for (const auto& symbol : config_.symbols) {
        states_[symbol].symbol = symbol;
        states_[symbol].bids.resize(params_.orderbook_levels);
        states_[symbol].asks.resize(params_.orderbook_levels);
        states_[symbol].prev_bids.resize(params_.orderbook_levels);
        states_[symbol].prev_asks.resize(params_.orderbook_levels);
    }
}

// ============================================================================
// Event Handlers
// ============================================================================

void OrderFlowImbalanceStrategy::on_market_data(const MarketData& data) {
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
        state.bids.resize(params_.orderbook_levels);
        state.asks.resize(params_.orderbook_levels);
        state.prev_bids.resize(params_.orderbook_levels);
        state.prev_asks.resize(params_.orderbook_levels);
    }

    // Store previous mid price
    state.prev_mid_price = state.bid_price > 0 && state.ask_price > 0 ?
        (state.bid_price + state.ask_price) / 2.0 : state.last_price;

    // Update basic prices
    state.last_price = data.last_price > 0 ? data.last_price : data.mid_price();
    state.bid_price = data.bid_price;
    state.ask_price = data.ask_price;
    state.exchange_id = data.exchange_id;

    // Update order book
    update_orderbook(state, data);

    // Calculate and store pressure
    auto [bid_pressure, ask_pressure] = calculate_pressure(state);
    PressurePoint pressure;
    pressure.bid_pressure = bid_pressure;
    pressure.ask_pressure = ask_pressure;
    pressure.net_pressure = bid_pressure - ask_pressure;
    pressure.timestamp = data.local_timestamp;
    state.pressure_history.push_back(pressure);
    while (state.pressure_history.size() > params_.pressure_window) {
        state.pressure_history.pop_front();
    }

    // Check exit conditions for existing positions
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

    // Check if spread is acceptable
    if (state.spread_bps() > params_.max_spread_bps) {
        return;  // Spread too wide
    }

    // Generate and evaluate signal
    FlowSignal signal = generate_signal(state);

    // Store signal in history
    state.signal_history.push_back(signal.combined_score);
    while (state.signal_history.size() > params_.signal_lookback) {
        state.signal_history.pop_front();
    }

    // Check entry conditions
    if (!state.has_position() && should_enter(state, signal)) {
        execute_entry(state, signal);
    }
}

void OrderFlowImbalanceStrategy::on_order_update(const OrderUpdate& update) {
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
        double fill_qty = update.filled_quantity;
        if (update.side == OrderSide::SELL) {
            fill_qty = -fill_qty;
        }

        if (std::abs(state.position_qty) < 1e-10) {
            // New position
            state.position_qty = fill_qty;
            state.entry_price = update.average_fill_price;
            state.entry_time = current_timestamp();
        } else if ((state.position_qty > 0 && fill_qty < 0) ||
                   (state.position_qty < 0 && fill_qty > 0)) {
            // Closing position
            double pnl = std::abs(state.position_qty) *
                        (update.average_fill_price - state.entry_price) *
                        (state.position_qty > 0 ? 1 : -1);
            state.total_pnl += pnl;
            state.total_signals++;
            if (pnl > 0) {
                state.successful_signals++;
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

void OrderFlowImbalanceStrategy::on_trade(const Trade& trade) {
    if (!is_running()) {
        return;
    }

    // Record trade for flow analysis
    auto state_it = states_.find(trade.symbol);
    if (state_it != states_.end()) {
        record_trade(state_it->second, trade);
    }

    update_position(trade);
}

// ============================================================================
// Analytics
// ============================================================================

OrderFlowImbalanceStrategy::FlowStats
OrderFlowImbalanceStrategy::get_flow_stats(const std::string& symbol) const {
    FlowStats stats;

    auto it = states_.find(symbol);
    if (it == states_.end()) {
        return stats;
    }

    const auto& state = it->second;

    stats.orderbook_imbalance = calculate_orderbook_imbalance(state);
    stats.multi_level_imbalance = calculate_multi_level_imbalance(state);

    auto [bid_p, ask_p] = calculate_pressure(state);
    stats.bid_pressure = bid_p;
    stats.ask_pressure = ask_p;

    stats.trade_imbalance = calculate_trade_imbalance(state);
    stats.aggressor_ratio = calculate_aggressor_ratio(state);
    stats.vpin = calculate_vpin(state);

    // Calculate signal strengths
    FlowSignal signal = generate_signal(state);
    stats.buy_signal_strength = signal.combined_score > 0 ? signal.combined_score : 0;
    stats.sell_signal_strength = signal.combined_score < 0 ? -signal.combined_score : 0;
    stats.net_signal = signal.combined_score;

    stats.total_signals = state.total_signals;
    stats.successful_signals = state.successful_signals;
    stats.total_pnl = state.total_pnl;

    return stats;
}

double OrderFlowImbalanceStrategy::get_vpin(const std::string& symbol) const {
    auto it = states_.find(symbol);
    if (it == states_.end()) {
        return 0.0;
    }
    return calculate_vpin(it->second);
}

double OrderFlowImbalanceStrategy::get_orderbook_imbalance(const std::string& symbol) const {
    auto it = states_.find(symbol);
    if (it == states_.end()) {
        return 0.0;
    }
    return calculate_orderbook_imbalance(it->second);
}

// ============================================================================
// Core Logic Implementation
// ============================================================================

void OrderFlowImbalanceStrategy::update_orderbook(SymbolState& state, const MarketData& data) {
    // Store previous state
    state.prev_bids = state.bids;
    state.prev_asks = state.asks;

    // Update from order book snapshot
    const auto& ob = data.orderbook;
    size_t levels = std::min(params_.orderbook_levels, static_cast<size_t>(ob.bid_levels));

    for (size_t i = 0; i < params_.orderbook_levels; ++i) {
        if (i < ob.bid_levels) {
            double prev_qty = state.prev_bids.size() > i ? state.prev_bids[i].quantity : 0.0;
            state.bids[i].price = ob.bids[i].price;
            state.bids[i].quantity = ob.bids[i].quantity;
            state.bids[i].quantity_change = ob.bids[i].quantity - prev_qty;
            state.bids[i].order_count = ob.bids[i].order_count;
        } else {
            state.bids[i] = OrderBookLevel{};
        }

        if (i < ob.ask_levels) {
            double prev_qty = state.prev_asks.size() > i ? state.prev_asks[i].quantity : 0.0;
            state.asks[i].price = ob.asks[i].price;
            state.asks[i].quantity = ob.asks[i].quantity;
            state.asks[i].quantity_change = ob.asks[i].quantity - prev_qty;
            state.asks[i].order_count = ob.asks[i].order_count;
        } else {
            state.asks[i] = OrderBookLevel{};
        }
    }
}

void OrderFlowImbalanceStrategy::record_trade(SymbolState& state, const Trade& trade) {
    TradeRecord record;
    record.price = trade.price;
    record.quantity = trade.quantity;
    record.timestamp = trade.timestamp;

    // Classify direction
    if (trade.is_maker) {
        // If trade is on maker side, aggressor is opposite
        record.aggressor_side = trade.side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
    } else {
        record.aggressor_side = trade.side;
    }

    record.is_buy = classify_trade_direction(state, trade.price);

    state.trades.push_back(record);
    while (state.trades.size() > params_.trade_window) {
        state.trades.pop_front();
    }

    // Update VPIN
    if (params_.use_vpin) {
        update_vpin(state, trade);
    }
}

double OrderFlowImbalanceStrategy::calculate_orderbook_imbalance(const SymbolState& state) const {
    // Simple top-of-book imbalance
    double bid_qty = 0.0;
    double ask_qty = 0.0;

    for (size_t i = 0; i < std::min(size_t(3), state.bids.size()); ++i) {
        bid_qty += state.bids[i].quantity;
    }
    for (size_t i = 0; i < std::min(size_t(3), state.asks.size()); ++i) {
        ask_qty += state.asks[i].quantity;
    }

    double total = bid_qty + ask_qty;
    if (total < 1e-10) {
        return 0.0;
    }

    return (bid_qty - ask_qty) / total;
}

double OrderFlowImbalanceStrategy::calculate_multi_level_imbalance(const SymbolState& state) const {
    double weighted_bid = 0.0;
    double weighted_ask = 0.0;
    double weight = 1.0;

    for (size_t i = 0; i < state.bids.size() && i < state.asks.size(); ++i) {
        weighted_bid += state.bids[i].quantity * weight;
        weighted_ask += state.asks[i].quantity * weight;
        weight *= params_.level_decay;
    }

    double total = weighted_bid + weighted_ask;
    if (total < 1e-10) {
        return 0.0;
    }

    return (weighted_bid - weighted_ask) / total;
}

std::pair<double, double>
OrderFlowImbalanceStrategy::calculate_pressure(const SymbolState& state) const {
    // Pressure based on order book changes
    double bid_pressure = 0.0;
    double ask_pressure = 0.0;

    for (size_t i = 0; i < state.bids.size(); ++i) {
        if (state.bids[i].quantity_change > 0) {
            bid_pressure += state.bids[i].quantity_change;
        }
    }

    for (size_t i = 0; i < state.asks.size(); ++i) {
        if (state.asks[i].quantity_change > 0) {
            ask_pressure += state.asks[i].quantity_change;
        }
    }

    // Normalize
    double max_pressure = std::max(bid_pressure + ask_pressure, 1.0);
    return {bid_pressure / max_pressure, ask_pressure / max_pressure};
}

void OrderFlowImbalanceStrategy::update_vpin(SymbolState& state, const Trade& trade) {
    // Add to current bucket
    if (trade.side == OrderSide::BUY || trade.is_maker == false) {
        state.current_bucket.buy_volume += trade.quantity;
    } else {
        state.current_bucket.sell_volume += trade.quantity;
    }
    state.current_bucket.trade_count++;

    if (state.current_bucket.start_time.count() == 0) {
        state.current_bucket.start_time = trade.timestamp;
    }
    state.current_bucket.end_time = trade.timestamp;

    // Check if bucket is full
    if (state.current_bucket.total_volume() >= params_.bucket_size) {
        state.vpin_buckets.push_back(state.current_bucket);
        while (state.vpin_buckets.size() > params_.vpin_buckets) {
            state.vpin_buckets.pop_front();
        }

        // Reset bucket
        state.current_bucket = VPINBucket{};
    }
}

double OrderFlowImbalanceStrategy::calculate_vpin(const SymbolState& state) const {
    if (state.vpin_buckets.empty()) {
        return 0.5;  // Neutral
    }

    double total_imbalance = 0.0;
    for (const auto& bucket : state.vpin_buckets) {
        total_imbalance += std::abs(bucket.imbalance());
    }

    return total_imbalance / state.vpin_buckets.size();
}

double OrderFlowImbalanceStrategy::calculate_trade_imbalance(const SymbolState& state) const {
    if (state.trades.empty()) {
        return 0.0;
    }

    double buy_volume = 0.0;
    double sell_volume = 0.0;

    for (const auto& trade : state.trades) {
        if (trade.is_buy) {
            buy_volume += trade.quantity;
        } else {
            sell_volume += trade.quantity;
        }
    }

    double total = buy_volume + sell_volume;
    if (total < 1e-10) {
        return 0.0;
    }

    return (buy_volume - sell_volume) / total;
}

double OrderFlowImbalanceStrategy::calculate_aggressor_ratio(const SymbolState& state) const {
    if (state.trades.empty()) {
        return 0.5;
    }

    int buy_aggressors = 0;
    int sell_aggressors = 0;

    for (const auto& trade : state.trades) {
        if (trade.aggressor_side == OrderSide::BUY) {
            buy_aggressors++;
        } else {
            sell_aggressors++;
        }
    }

    int total = buy_aggressors + sell_aggressors;
    if (total == 0) {
        return 0.5;
    }

    return static_cast<double>(buy_aggressors) / total;
}

OrderFlowImbalanceStrategy::FlowSignal
OrderFlowImbalanceStrategy::generate_signal(const SymbolState& state) const {
    FlowSignal signal;

    // Calculate individual scores (-1 to 1 range)

    // Order book imbalance
    if (params_.use_multi_level_imbalance) {
        signal.orderbook_score = calculate_multi_level_imbalance(state);
    } else {
        signal.orderbook_score = calculate_orderbook_imbalance(state);
    }

    // Trade flow
    if (params_.use_trade_flow) {
        double trade_imb = calculate_trade_imbalance(state);
        double aggressor = calculate_aggressor_ratio(state);
        signal.trade_flow_score = (trade_imb + (aggressor * 2 - 1)) / 2.0;
    }

    // VPIN (toxicity indicator)
    if (params_.use_vpin) {
        double vpin = calculate_vpin(state);
        // High VPIN with directional bias
        double trade_imb = calculate_trade_imbalance(state);
        signal.vpin_score = vpin * trade_imb;
    }

    // Pressure
    if (params_.use_orderbook_pressure && !state.pressure_history.empty()) {
        double avg_net_pressure = 0.0;
        for (const auto& p : state.pressure_history) {
            avg_net_pressure += p.net_pressure;
        }
        avg_net_pressure /= state.pressure_history.size();
        signal.pressure_score = avg_net_pressure;
    }

    // Combine scores
    double weight_sum = 0.0;
    signal.combined_score = 0.0;

    if (params_.use_multi_level_imbalance) {
        signal.combined_score += signal.orderbook_score * 2.0;
        weight_sum += 2.0;
    }
    if (params_.use_trade_flow) {
        signal.combined_score += signal.trade_flow_score * 1.5;
        weight_sum += 1.5;
    }
    if (params_.use_vpin) {
        signal.combined_score += signal.vpin_score * 1.0;
        weight_sum += 1.0;
    }
    if (params_.use_orderbook_pressure) {
        signal.combined_score += signal.pressure_score * 1.0;
        weight_sum += 1.0;
    }

    if (weight_sum > 0) {
        signal.combined_score /= weight_sum;
    }

    // Apply signal decay based on recent signals
    if (!state.signal_history.empty()) {
        double recent_signal_sum = 0.0;
        double weight = 1.0;
        double total_weight = 0.0;
        for (auto it = state.signal_history.rbegin(); it != state.signal_history.rend(); ++it) {
            recent_signal_sum += *it * weight;
            total_weight += weight;
            weight *= params_.signal_decay;
        }
        double recent_trend = recent_signal_sum / total_weight;

        // Boost signal if consistent with recent trend
        if ((signal.combined_score > 0 && recent_trend > 0) ||
            (signal.combined_score < 0 && recent_trend < 0)) {
            signal.combined_score *= 1.2;
        }
    }

    // Determine signal type
    if (signal.combined_score > params_.imbalance_threshold) {
        signal.type = SignalType::BUY;
        signal.reason = "Bullish flow imbalance: OB=" +
                       std::to_string(signal.orderbook_score) +
                       " TF=" + std::to_string(signal.trade_flow_score) +
                       " Combined=" + std::to_string(signal.combined_score);
    } else if (signal.combined_score < -params_.imbalance_threshold) {
        signal.type = SignalType::SELL;
        signal.reason = "Bearish flow imbalance: OB=" +
                       std::to_string(signal.orderbook_score) +
                       " TF=" + std::to_string(signal.trade_flow_score) +
                       " Combined=" + std::to_string(signal.combined_score);
    }

    // Calculate confidence
    signal.confidence = calculate_confidence(signal, state);

    return signal;
}

double OrderFlowImbalanceStrategy::calculate_confidence(
    const FlowSignal& signal, const SymbolState& state) const {

    double confidence = 0.5;

    // Score magnitude
    double score_magnitude = std::abs(signal.combined_score);
    confidence += std::min(0.25, score_magnitude * 0.5);

    // Signal alignment (multiple indicators agreeing)
    int aligned = 0;
    if (signal.combined_score > 0) {
        if (signal.orderbook_score > 0) aligned++;
        if (signal.trade_flow_score > 0) aligned++;
        if (signal.vpin_score > 0) aligned++;
        if (signal.pressure_score > 0) aligned++;
    } else if (signal.combined_score < 0) {
        if (signal.orderbook_score < 0) aligned++;
        if (signal.trade_flow_score < 0) aligned++;
        if (signal.vpin_score < 0) aligned++;
        if (signal.pressure_score < 0) aligned++;
    }
    confidence += aligned * 0.05;

    // VPIN toxicity (higher VPIN = more informed trading)
    if (params_.use_vpin) {
        double vpin = calculate_vpin(state);
        if (vpin > params_.vpin_threshold) {
            confidence += 0.1;
        }
    }

    // Reduce confidence for wide spreads
    double spread = state.spread_bps();
    if (spread > params_.max_spread_bps * 0.5) {
        confidence *= 0.8;
    }

    return std::clamp(confidence, 0.0, 1.0);
}

bool OrderFlowImbalanceStrategy::should_enter(
    const SymbolState& state, const FlowSignal& signal) const {

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
        if ((now - it->second).count() < params_.min_signal_interval_us * 1000) {
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

bool OrderFlowImbalanceStrategy::should_exit(const SymbolState& state) const {
    if (!state.has_position()) {
        return false;
    }

    // Check if flow reversed
    double imbalance = calculate_orderbook_imbalance(state);

    if (state.is_long() && imbalance < -params_.imbalance_threshold) {
        return true;  // Flow reversed against long
    }
    if (state.is_short() && imbalance > params_.imbalance_threshold) {
        return true;  // Flow reversed against short
    }

    return false;
}

double OrderFlowImbalanceStrategy::calculate_size(
    const SymbolState& state, const FlowSignal& signal) const {

    double base_size = params_.max_position * params_.position_size_pct;

    // Scale by confidence
    base_size *= signal.confidence;

    // Scale by signal strength
    double strength_factor = std::min(2.0, std::abs(signal.combined_score) / params_.imbalance_threshold);
    base_size *= (0.5 + 0.5 * strength_factor);

    return std::min(base_size, params_.max_position);
}

void OrderFlowImbalanceStrategy::execute_entry(
    SymbolState& state, const FlowSignal& signal) {

    double size = calculate_size(state, signal);

    Signal order_signal;
    order_signal.type = signal.type;
    order_signal.symbol = state.symbol;
    order_signal.exchange = params_.exchange;
    order_signal.exchange_id = state.exchange_id;
    order_signal.target_quantity = size;
    order_signal.confidence = signal.confidence;
    order_signal.reason = signal.reason;
    order_signal.urgency = 0.8;

    if (signal.type == SignalType::BUY) {
        order_signal.target_price = state.ask_price;
        order_signal.stop_loss = state.last_price * (1.0 - params_.stop_loss_pct);
        order_signal.take_profit = state.last_price * (1.0 + params_.take_profit_pct);
    } else {
        order_signal.target_price = state.bid_price;
        order_signal.stop_loss = state.last_price * (1.0 + params_.stop_loss_pct);
        order_signal.take_profit = state.last_price * (1.0 - params_.take_profit_pct);
    }

    order_signal.timeout_us = params_.max_hold_time_us;

    uint64_t order_id = current_timestamp().count();
    order_to_symbol_[order_id] = state.symbol;
    last_signal_time_[state.symbol] = current_timestamp();

    emit_signal(std::move(order_signal));
}

void OrderFlowImbalanceStrategy::execute_exit(
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
    order_signal.urgency = 1.0;
    order_signal.confidence = 1.0;

    uint64_t order_id = current_timestamp().count();
    order_to_symbol_[order_id] = state.symbol;

    emit_signal(std::move(order_signal));
}

void OrderFlowImbalanceStrategy::check_exit_conditions(SymbolState& state) {
    if (!state.has_position()) {
        return;
    }

    double current_pnl_pct;
    if (state.is_long()) {
        current_pnl_pct = (state.last_price - state.entry_price) / state.entry_price;
    } else {
        current_pnl_pct = (state.entry_price - state.last_price) / state.entry_price;
    }

    // Stop loss
    if (current_pnl_pct < -params_.stop_loss_pct) {
        execute_exit(state, "Stop loss hit: " + std::to_string(current_pnl_pct * 100) + "%");
        return;
    }

    // Take profit
    if (current_pnl_pct > params_.take_profit_pct) {
        execute_exit(state, "Take profit hit: " + std::to_string(current_pnl_pct * 100) + "%");
        return;
    }

    // Flow reversal
    if (should_exit(state)) {
        execute_exit(state, "Flow reversal detected");
        return;
    }
}

bool OrderFlowImbalanceStrategy::classify_trade_direction(
    const SymbolState& state, Price price) const {
    // Tick rule: if price > prev mid, it's a buy
    return price > state.prev_mid_price;
}

}  // namespace strategies
}  // namespace hft
