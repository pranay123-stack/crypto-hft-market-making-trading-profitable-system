#include "strategies/hft/latency_arbitrage.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hft {
namespace strategies {

// ============================================================================
// Constructor
// ============================================================================

LatencyArbitrageStrategy::LatencyArbitrageStrategy(StrategyConfig config)
    : StrategyBase(std::move(config))
{
    // Load parameters from config
    params_.min_spread_bps = config_.get_param<double>("min_spread_bps", 5.0);
    params_.max_position = config_.get_param<double>("max_position", 1.0);
    params_.timeout_us = static_cast<int64_t>(config_.get_param<double>("timeout_us", 1000000.0));
    params_.min_confidence = config_.get_param<double>("min_confidence", 0.7);
    params_.max_latency_diff_ns = static_cast<int64_t>(config_.get_param<double>("max_latency_diff_ns", 50000000.0));
    params_.min_trade_size = config_.get_param<double>("min_trade_size", 0.001);
    params_.max_slippage_bps = config_.get_param<double>("max_slippage_bps", 2.0);
    params_.track_spreads = config_.get_param<bool>("track_spreads", true);
    params_.spread_window_size = static_cast<size_t>(config_.get_param<double>("spread_window_size", 100.0));

    // Default fees for common exchanges
    params_.exchange_fees["binance"] = 0.001;      // 0.1%
    params_.exchange_fees["coinbase"] = 0.005;    // 0.5%
    params_.exchange_fees["kraken"] = 0.0026;     // 0.26%
    params_.exchange_fees["ftx"] = 0.0007;        // 0.07%
    params_.exchange_fees["bybit"] = 0.001;       // 0.1%
    params_.exchange_fees["okx"] = 0.001;         // 0.1%
    params_.exchange_fees["huobi"] = 0.002;       // 0.2%
    params_.exchange_fees["default"] = 0.001;     // 0.1% default
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

bool LatencyArbitrageStrategy::initialize() {
    if (!StrategyBase::initialize()) {
        return false;
    }

    // Pre-allocate space for symbols
    for (const auto& symbol : config_.symbols) {
        quotes_[symbol] = {};
        spread_history_[symbol] = SpreadHistory{};
    }

    return true;
}

void LatencyArbitrageStrategy::on_stop() {
    // Cancel any pending arbitrages
    pending_arbitrages_.clear();
    order_to_arbitrage_.clear();
}

void LatencyArbitrageStrategy::on_reset() {
    quotes_.clear();
    pending_arbitrages_.clear();
    order_to_arbitrage_.clear();
    spread_history_.clear();
    last_check_time_.clear();

    for (const auto& symbol : config_.symbols) {
        quotes_[symbol] = {};
        spread_history_[symbol] = SpreadHistory{};
    }
}

// ============================================================================
// Event Handlers
// ============================================================================

void LatencyArbitrageStrategy::on_market_data(const MarketData& data) {
    if (!is_running()) {
        return;
    }

    // Update backtest timestamp if needed
    if (is_backtest_mode_) {
        set_backtest_timestamp(data.local_timestamp);
    }

    // Update the quote
    update_quote(data.symbol, data);

    // Check for timeout on pending arbitrages
    check_pending_timeouts();

    // Rate-limit opportunity detection
    auto now = current_timestamp();
    auto& last_check = last_check_time_[data.symbol];
    if ((now - last_check).count() < MIN_CHECK_INTERVAL_NS) {
        return;
    }
    last_check = now;

    // Detect and evaluate opportunities
    auto opportunities = detect_opportunities(data.symbol);

    for (const auto& opp : opportunities) {
        if (opp.is_profitable() && opp.confidence >= params_.min_confidence) {
            evaluate_opportunity(opp);
        }
    }
}

void LatencyArbitrageStrategy::on_order_update(const OrderUpdate& update) {
    if (!is_running()) {
        return;
    }

    // Find if this order is part of an arbitrage
    auto it = order_to_arbitrage_.find(update.order_id);
    if (it == order_to_arbitrage_.end()) {
        return;
    }

    uint64_t arb_id = it->second;
    auto arb_it = pending_arbitrages_.find(arb_id);
    if (arb_it == pending_arbitrages_.end()) {
        return;
    }

    auto& arb = arb_it->second;

    // Update fill status
    if (update.order_id == arb.buy_order_id) {
        if (update.status == OrderStatus::FILLED || update.status == OrderStatus::PARTIALLY_FILLED) {
            arb.buy_filled_qty = update.filled_quantity;
            arb.buy_avg_price = update.average_fill_price;
            arb.buy_filled = (update.status == OrderStatus::FILLED);
        } else if (update.is_terminal()) {
            // Buy order failed/cancelled
            // Need to potentially cancel sell order if not filled
            if (!arb.sell_filled) {
                Signal cancel_signal;
                cancel_signal.type = SignalType::CLOSE_LONG;
                cancel_signal.symbol = arb.opportunity.symbol;
                cancel_signal.exchange = arb.opportunity.sell_exchange;
                cancel_signal.reason = "Arbitrage buy leg failed";
                emit_signal(std::move(cancel_signal));
            }
        }
    } else if (update.order_id == arb.sell_order_id) {
        if (update.status == OrderStatus::FILLED || update.status == OrderStatus::PARTIALLY_FILLED) {
            arb.sell_filled_qty = update.filled_quantity;
            arb.sell_avg_price = update.average_fill_price;
            arb.sell_filled = (update.status == OrderStatus::FILLED);
        } else if (update.is_terminal()) {
            // Sell order failed/cancelled
            if (!arb.buy_filled) {
                Signal cancel_signal;
                cancel_signal.type = SignalType::CLOSE_SHORT;
                cancel_signal.symbol = arb.opportunity.symbol;
                cancel_signal.exchange = arb.opportunity.buy_exchange;
                cancel_signal.reason = "Arbitrage sell leg failed";
                emit_signal(std::move(cancel_signal));
            }
        }
    }

    // Check if arbitrage is complete
    if (arb.buy_filled && arb.sell_filled) {
        // Calculate realized profit
        double profit = (arb.sell_avg_price - arb.buy_avg_price) *
                       std::min(arb.buy_filled_qty, arb.sell_filled_qty);

        // Subtract fees
        double buy_fee = arb.buy_filled_qty * arb.buy_avg_price *
                        get_exchange_fee(arb.opportunity.buy_exchange);
        double sell_fee = arb.sell_filled_qty * arb.sell_avg_price *
                         get_exchange_fee(arb.opportunity.sell_exchange);
        profit -= (buy_fee + sell_fee);

        // Update statistics
        auto& history = spread_history_[arb.opportunity.symbol];
        history.execution_count++;
        history.total_profit += profit;

        // Clean up
        order_to_arbitrage_.erase(arb.buy_order_id);
        order_to_arbitrage_.erase(arb.sell_order_id);
        pending_arbitrages_.erase(arb_it);
    }
}

void LatencyArbitrageStrategy::on_trade(const Trade& trade) {
    if (!is_running()) {
        return;
    }

    // Update position tracking
    update_position(trade);
    update_arbitrage_position(trade);
}

// ============================================================================
// Analytics
// ============================================================================

LatencyArbitrageStrategy::SpreadStats
LatencyArbitrageStrategy::get_spread_stats(const std::string& symbol) const {
    SpreadStats stats;

    auto it = spread_history_.find(symbol);
    if (it != spread_history_.end()) {
        const auto& history = it->second;
        stats.mean_spread_bps = history.mean();
        stats.std_spread_bps = history.stddev();
        stats.max_spread_bps = history.max_spread;
        stats.min_spread_bps = history.min_spread;
        stats.arbitrage_opportunities = history.opportunity_count;
        stats.executed_arbitrages = history.execution_count;
        stats.total_profit = history.total_profit;
    }

    return stats;
}

double LatencyArbitrageStrategy::get_current_spread(const std::string& symbol) const {
    auto it = quotes_.find(symbol);
    if (it == quotes_.end() || it->second.size() < 2) {
        return 0.0;
    }

    const auto& exchange_quotes = it->second;

    // Find best bid and best ask across all exchanges
    double best_bid = 0.0;
    double best_ask = std::numeric_limits<double>::max();

    for (const auto& [exchange, quote] : exchange_quotes) {
        if (!quote.is_valid()) continue;
        best_bid = std::max(best_bid, quote.bid_price);
        best_ask = std::min(best_ask, quote.ask_price);
    }

    if (best_bid > 0 && best_ask < std::numeric_limits<double>::max() && best_bid > best_ask) {
        double mid = (best_bid + best_ask) / 2.0;
        return ((best_bid - best_ask) / mid) * 10000.0;  // Convert to bps
    }

    return 0.0;
}

// ============================================================================
// Core Logic Implementation
// ============================================================================

void LatencyArbitrageStrategy::update_quote(const std::string& symbol, const MarketData& data) {
    auto& quote = quotes_[symbol][data.exchange];

    quote.exchange = data.exchange;
    quote.exchange_id = data.exchange_id;
    quote.bid_price = data.bid_price;
    quote.ask_price = data.ask_price;
    quote.bid_size = data.bid_size;
    quote.ask_size = data.ask_size;
    quote.timestamp = data.exchange_timestamp;
    quote.local_timestamp = data.local_timestamp;
    quote.latency_ns = data.latency_ns();
}

std::vector<LatencyArbitrageStrategy::ArbitrageOpportunity>
LatencyArbitrageStrategy::detect_opportunities(const std::string& symbol) {
    std::vector<ArbitrageOpportunity> opportunities;

    auto it = quotes_.find(symbol);
    if (it == quotes_.end() || it->second.size() < 2) {
        return opportunities;
    }

    const auto& exchange_quotes = it->second;
    auto now = current_timestamp();

    // Compare all pairs of exchanges
    for (auto it1 = exchange_quotes.begin(); it1 != exchange_quotes.end(); ++it1) {
        const auto& [exchange1, quote1] = *it1;

        // Skip stale or invalid quotes
        if (!quote1.is_valid() || quote1.is_stale(now, params_.max_latency_diff_ns * 2)) {
            continue;
        }

        for (auto it2 = std::next(it1); it2 != exchange_quotes.end(); ++it2) {
            const auto& [exchange2, quote2] = *it2;

            if (!quote2.is_valid() || quote2.is_stale(now, params_.max_latency_diff_ns * 2)) {
                continue;
            }

            // Check if quotes are synchronized
            if (!are_quotes_synchronized(quote1, quote2)) {
                continue;
            }

            // Case 1: Buy on exchange1 (at ask), sell on exchange2 (at bid)
            if (quote2.bid_price > quote1.ask_price) {
                ArbitrageOpportunity opp;
                opp.symbol = symbol;
                opp.buy_exchange = exchange1;
                opp.sell_exchange = exchange2;
                opp.buy_exchange_id = quote1.exchange_id;
                opp.sell_exchange_id = quote2.exchange_id;
                opp.buy_price = quote1.ask_price;
                opp.sell_price = quote2.bid_price;
                opp.max_size = std::min({
                    quote1.ask_size,
                    quote2.bid_size,
                    params_.max_position
                });

                double mid = (opp.buy_price + opp.sell_price) / 2.0;
                opp.gross_spread_bps = ((opp.sell_price - opp.buy_price) / mid) * 10000.0;
                opp.net_spread_bps = calculate_net_spread(exchange1, exchange2,
                                                          opp.buy_price, opp.sell_price);
                opp.expected_profit = opp.max_size * (opp.sell_price - opp.buy_price) *
                                     (1.0 - get_exchange_fee(exchange1) - get_exchange_fee(exchange2));
                opp.confidence = calculate_confidence(opp, quote1, quote2);
                opp.detected_at = now;
                opp.timeout_us = params_.timeout_us;

                // Track spread statistics
                if (params_.track_spreads) {
                    spread_history_[symbol].add_spread(opp.gross_spread_bps, params_.spread_window_size);
                    if (opp.net_spread_bps > params_.min_spread_bps) {
                        spread_history_[symbol].opportunity_count++;
                    }
                }

                if (opp.net_spread_bps >= params_.min_spread_bps && opp.max_size >= params_.min_trade_size) {
                    opportunities.push_back(std::move(opp));
                }
            }

            // Case 2: Buy on exchange2 (at ask), sell on exchange1 (at bid)
            if (quote1.bid_price > quote2.ask_price) {
                ArbitrageOpportunity opp;
                opp.symbol = symbol;
                opp.buy_exchange = exchange2;
                opp.sell_exchange = exchange1;
                opp.buy_exchange_id = quote2.exchange_id;
                opp.sell_exchange_id = quote1.exchange_id;
                opp.buy_price = quote2.ask_price;
                opp.sell_price = quote1.bid_price;
                opp.max_size = std::min({
                    quote2.ask_size,
                    quote1.bid_size,
                    params_.max_position
                });

                double mid = (opp.buy_price + opp.sell_price) / 2.0;
                opp.gross_spread_bps = ((opp.sell_price - opp.buy_price) / mid) * 10000.0;
                opp.net_spread_bps = calculate_net_spread(exchange2, exchange1,
                                                          opp.buy_price, opp.sell_price);
                opp.expected_profit = opp.max_size * (opp.sell_price - opp.buy_price) *
                                     (1.0 - get_exchange_fee(exchange1) - get_exchange_fee(exchange2));
                opp.confidence = calculate_confidence(opp, quote2, quote1);
                opp.detected_at = now;
                opp.timeout_us = params_.timeout_us;

                if (params_.track_spreads) {
                    spread_history_[symbol].add_spread(opp.gross_spread_bps, params_.spread_window_size);
                    if (opp.net_spread_bps > params_.min_spread_bps) {
                        spread_history_[symbol].opportunity_count++;
                    }
                }

                if (opp.net_spread_bps >= params_.min_spread_bps && opp.max_size >= params_.min_trade_size) {
                    opportunities.push_back(std::move(opp));
                }
            }
        }
    }

    // Sort by expected profit (descending)
    std::sort(opportunities.begin(), opportunities.end(),
              [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                  return a.expected_profit > b.expected_profit;
              });

    return opportunities;
}

void LatencyArbitrageStrategy::evaluate_opportunity(const ArbitrageOpportunity& opp) {
    // Check risk limits
    double notional = opp.max_size * opp.buy_price;
    if (!can_open_position(notional)) {
        return;
    }

    // Check if we already have pending arbitrages for this symbol
    for (const auto& [id, pending] : pending_arbitrages_) {
        if (pending.opportunity.symbol == opp.symbol) {
            // Already have a pending arbitrage for this symbol
            return;
        }
    }

    // Check current positions
    auto pos1 = get_position(opp.symbol + "_" + opp.buy_exchange);
    auto pos2 = get_position(opp.symbol + "_" + opp.sell_exchange);

    double current_exposure = 0.0;
    if (pos1) current_exposure += std::abs(pos1->quantity);
    if (pos2) current_exposure += std::abs(pos2->quantity);

    if (current_exposure + opp.max_size > params_.max_position) {
        return;
    }

    // Execute the arbitrage
    execute_arbitrage(opp);
}

void LatencyArbitrageStrategy::execute_arbitrage(const ArbitrageOpportunity& opp) {
    // Generate unique arbitrage ID
    uint64_t arb_id = next_arb_id_.fetch_add(1, std::memory_order_relaxed);

    // Create buy signal
    Signal buy_signal;
    buy_signal.type = SignalType::BUY;
    buy_signal.symbol = opp.symbol;
    buy_signal.exchange = opp.buy_exchange;
    buy_signal.exchange_id = opp.buy_exchange_id;
    buy_signal.target_price = opp.buy_price * (1.0 + params_.max_slippage_bps / 10000.0);
    buy_signal.target_quantity = opp.max_size;
    buy_signal.confidence = opp.confidence;
    buy_signal.urgency = 1.0;  // Maximum urgency for arbitrage
    buy_signal.timeout_us = opp.timeout_us;
    buy_signal.reason = "Latency arbitrage buy leg: spread=" +
                       std::to_string(opp.net_spread_bps) + "bps";

    // Create sell signal
    Signal sell_signal;
    sell_signal.type = SignalType::SELL;
    sell_signal.symbol = opp.symbol;
    sell_signal.exchange = opp.sell_exchange;
    sell_signal.exchange_id = opp.sell_exchange_id;
    sell_signal.target_price = opp.sell_price * (1.0 - params_.max_slippage_bps / 10000.0);
    sell_signal.target_quantity = opp.max_size;
    sell_signal.confidence = opp.confidence;
    sell_signal.urgency = 1.0;
    sell_signal.timeout_us = opp.timeout_us;
    sell_signal.reason = "Latency arbitrage sell leg: spread=" +
                        std::to_string(opp.net_spread_bps) + "bps";

    // Store pending arbitrage
    PendingArbitrage pending;
    pending.opportunity = opp;
    pending.buy_order_id = arb_id * 2;      // Even ID for buy
    pending.sell_order_id = arb_id * 2 + 1; // Odd ID for sell
    pending.start_time = current_timestamp();

    order_to_arbitrage_[pending.buy_order_id] = arb_id;
    order_to_arbitrage_[pending.sell_order_id] = arb_id;
    pending_arbitrages_[arb_id] = std::move(pending);

    // Emit signals
    emit_signal(std::move(buy_signal));
    emit_signal(std::move(sell_signal));
}

double LatencyArbitrageStrategy::calculate_net_spread(
    const std::string& buy_exchange,
    const std::string& sell_exchange,
    Price buy_price,
    Price sell_price) const
{
    double buy_fee = get_exchange_fee(buy_exchange);
    double sell_fee = get_exchange_fee(sell_exchange);

    // Effective prices after fees
    double effective_buy = buy_price * (1.0 + buy_fee);
    double effective_sell = sell_price * (1.0 - sell_fee);

    double mid = (buy_price + sell_price) / 2.0;
    return ((effective_sell - effective_buy) / mid) * 10000.0;
}

double LatencyArbitrageStrategy::get_exchange_fee(const std::string& exchange) const {
    auto it = params_.exchange_fees.find(exchange);
    if (it != params_.exchange_fees.end()) {
        return it->second;
    }
    return params_.exchange_fees.at("default");
}

bool LatencyArbitrageStrategy::are_quotes_synchronized(
    const ExchangeQuote& q1,
    const ExchangeQuote& q2) const
{
    int64_t time_diff = std::abs((q1.local_timestamp - q2.local_timestamp).count());
    return time_diff <= params_.max_latency_diff_ns;
}

double LatencyArbitrageStrategy::calculate_confidence(
    const ArbitrageOpportunity& opp,
    const ExchangeQuote& buy_quote,
    const ExchangeQuote& sell_quote) const
{
    double confidence = 1.0;

    // Reduce confidence based on spread-to-fees ratio
    double fee_cost_bps = (get_exchange_fee(opp.buy_exchange) +
                           get_exchange_fee(opp.sell_exchange)) * 10000.0;
    double spread_margin = opp.gross_spread_bps - fee_cost_bps;
    if (spread_margin < params_.min_spread_bps * 2) {
        confidence *= 0.5 + 0.5 * (spread_margin / (params_.min_spread_bps * 2));
    }

    // Reduce confidence based on quote staleness
    auto now = current_timestamp();
    int64_t buy_age = (now - buy_quote.local_timestamp).count();
    int64_t sell_age = (now - sell_quote.local_timestamp).count();
    int64_t max_age = params_.max_latency_diff_ns;

    double freshness = 1.0 - (std::max(buy_age, sell_age) / static_cast<double>(max_age));
    confidence *= std::max(0.0, freshness);

    // Reduce confidence based on available size
    if (opp.max_size < params_.min_trade_size * 10) {
        confidence *= opp.max_size / (params_.min_trade_size * 10);
    }

    // Reduce confidence based on latency asymmetry
    int64_t latency_diff = std::abs(buy_quote.latency_ns - sell_quote.latency_ns);
    if (latency_diff > params_.max_latency_diff_ns / 2) {
        confidence *= 0.5;
    }

    return std::clamp(confidence, 0.0, 1.0);
}

void LatencyArbitrageStrategy::check_pending_timeouts() {
    auto now = current_timestamp();
    std::vector<uint64_t> to_remove;

    for (auto& [arb_id, pending] : pending_arbitrages_) {
        if (pending.opportunity.is_expired(now)) {
            // Timeout - need to close any open positions
            if (pending.buy_filled && !pending.sell_filled) {
                // Buy filled but sell didn't - need to sell what we bought
                Signal close_signal;
                close_signal.type = SignalType::SELL;
                close_signal.symbol = pending.opportunity.symbol;
                close_signal.exchange = pending.opportunity.buy_exchange;
                close_signal.target_quantity = pending.buy_filled_qty;
                close_signal.urgency = 1.0;
                close_signal.reason = "Arbitrage timeout - closing buy leg";
                emit_signal(std::move(close_signal));
            } else if (!pending.buy_filled && pending.sell_filled) {
                // Sell filled but buy didn't - need to buy to cover
                Signal close_signal;
                close_signal.type = SignalType::BUY;
                close_signal.symbol = pending.opportunity.symbol;
                close_signal.exchange = pending.opportunity.sell_exchange;
                close_signal.target_quantity = pending.sell_filled_qty;
                close_signal.urgency = 1.0;
                close_signal.reason = "Arbitrage timeout - closing sell leg";
                emit_signal(std::move(close_signal));
            }

            to_remove.push_back(arb_id);
        }
    }

    for (uint64_t arb_id : to_remove) {
        auto& pending = pending_arbitrages_[arb_id];
        order_to_arbitrage_.erase(pending.buy_order_id);
        order_to_arbitrage_.erase(pending.sell_order_id);
        pending_arbitrages_.erase(arb_id);
    }
}

void LatencyArbitrageStrategy::update_arbitrage_position(const Trade& trade) {
    // Additional arbitrage-specific position tracking if needed
    // Base class already handles general position tracking
}

}  // namespace strategies
}  // namespace hft
