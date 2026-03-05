#include "strategies/hft/triangular_arbitrage.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace hft {
namespace strategies {

// ============================================================================
// Constructor
// ============================================================================

TriangularArbitrageStrategy::TriangularArbitrageStrategy(StrategyConfig config)
    : StrategyBase(std::move(config))
{
    // Load parameters from config
    params_.min_profit_bps = config_.get_param<double>("min_profit_bps", 3.0);
    params_.max_position = config_.get_param<double>("max_position", 0.5);
    params_.execution_timeout_us = static_cast<int64_t>(
        config_.get_param<double>("execution_timeout_us", 500000.0));
    params_.min_confidence = config_.get_param<double>("min_confidence", 0.8);
    params_.max_quote_age_ns = static_cast<int64_t>(
        config_.get_param<double>("max_quote_age_ns", 100000000.0));
    params_.fee_rate = config_.get_param<double>("fee_rate", 0.001);
    params_.min_trade_size = config_.get_param<double>("min_trade_size", 0.001);
    params_.slippage_tolerance_bps = config_.get_param<double>("slippage_tolerance_bps", 1.0);
    params_.exchange = config_.get_param<std::string>("exchange", std::string("binance"));
    params_.check_reverse_paths = config_.get_param<bool>("check_reverse_paths", true);

    // Common trading pair mappings
    pair_to_symbol_["BTC_ETH"] = "ETHBTC";
    pair_to_symbol_["ETH_BTC"] = "ETHBTC";
    pair_to_symbol_["BTC_USDT"] = "BTCUSDT";
    pair_to_symbol_["USDT_BTC"] = "BTCUSDT";
    pair_to_symbol_["ETH_USDT"] = "ETHUSDT";
    pair_to_symbol_["USDT_ETH"] = "ETHUSDT";
    pair_to_symbol_["BNB_BTC"] = "BNBBTC";
    pair_to_symbol_["BTC_BNB"] = "BNBBTC";
    pair_to_symbol_["BNB_USDT"] = "BNBUSDT";
    pair_to_symbol_["USDT_BNB"] = "BNBUSDT";
    pair_to_symbol_["BNB_ETH"] = "BNBETH";
    pair_to_symbol_["ETH_BNB"] = "BNBETH";
    pair_to_symbol_["SOL_BTC"] = "SOLBTC";
    pair_to_symbol_["SOL_USDT"] = "SOLUSDT";
    pair_to_symbol_["SOL_ETH"] = "SOLETH";
    pair_to_symbol_["XRP_BTC"] = "XRPBTC";
    pair_to_symbol_["XRP_USDT"] = "XRPUSDT";
    pair_to_symbol_["XRP_ETH"] = "XRPETH";
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

bool TriangularArbitrageStrategy::initialize() {
    if (!StrategyBase::initialize()) {
        return false;
    }

    // Set up default triangles if currencies specified
    if (!params_.triangle_currencies.empty() && params_.triangle_currencies.size() >= 3) {
        // Create triangles from all combinations
        const auto& currencies = params_.triangle_currencies;
        for (size_t i = 0; i < currencies.size(); ++i) {
            for (size_t j = i + 1; j < currencies.size(); ++j) {
                for (size_t k = j + 1; k < currencies.size(); ++k) {
                    add_triangle(currencies[i], currencies[j], currencies[k]);
                }
            }
        }
    }

    // Default triangle: BTC-ETH-USDT
    if (triangles_.empty()) {
        add_triangle("BTC", "ETH", "USDT");
    }

    return true;
}

void TriangularArbitrageStrategy::on_stop() {
    // Clear pending executions
    pending_triangles_.clear();
    order_to_triangle_.clear();
}

void TriangularArbitrageStrategy::on_reset() {
    pairs_.clear();
    pending_triangles_.clear();
    order_to_triangle_.clear();

    // Reset triangle statistics
    for (auto& triangle : triangles_) {
        triangle.opportunities_count = 0;
        triangle.executions_count = 0;
        triangle.total_profit = 0.0;
        triangle.recent_profits.clear();
    }
}

// ============================================================================
// Event Handlers
// ============================================================================

void TriangularArbitrageStrategy::on_market_data(const MarketData& data) {
    if (!is_running()) {
        return;
    }

    // Update backtest timestamp
    if (is_backtest_mode_) {
        set_backtest_timestamp(data.local_timestamp);
    }

    // Update pair data
    update_pair_data(data);

    // Check pending timeouts
    check_pending_timeouts();

    // Rate limit checks
    auto now = current_timestamp();
    if ((now - last_check_time_).count() < MIN_CHECK_INTERVAL_NS) {
        return;
    }
    last_check_time_ = now;

    // Check each triangle for opportunities
    for (auto& triangle : triangles_) {
        if (!has_fresh_data(triangle)) {
            continue;
        }

        auto opportunity = calculate_opportunity(triangle);
        if (opportunity && opportunity->is_profitable() &&
            opportunity->best_profit_bps >= params_.min_profit_bps &&
            opportunity->confidence >= params_.min_confidence) {

            // Check risk limits
            double notional = opportunity->max_size * opportunity->leg1_price;
            if (can_open_position(notional)) {
                execute_triangle(*opportunity);
            }
        }
    }
}

void TriangularArbitrageStrategy::on_order_update(const OrderUpdate& update) {
    if (!is_running()) {
        return;
    }

    // Find if this order is part of a triangle
    auto it = order_to_triangle_.find(update.order_id);
    if (it == order_to_triangle_.end()) {
        return;
    }

    uint64_t triangle_id = it->second;
    auto tri_it = pending_triangles_.find(triangle_id);
    if (tri_it == pending_triangles_.end()) {
        return;
    }

    auto& pending = tri_it->second;

    // Update the appropriate leg
    if (update.order_id == pending.leg1_order_id) {
        if (update.status == OrderStatus::FILLED) {
            pending.leg1_filled = true;
            pending.leg1_filled_qty = update.filled_quantity;
            pending.leg1_avg_price = update.average_fill_price;
            pending.current_leg = 2;
            execute_next_leg(pending);
        } else if (update.is_terminal() && update.status != OrderStatus::FILLED) {
            // Leg 1 failed - abort
            pending_triangles_.erase(tri_it);
            order_to_triangle_.erase(update.order_id);
        }
    } else if (update.order_id == pending.leg2_order_id) {
        if (update.status == OrderStatus::FILLED) {
            pending.leg2_filled = true;
            pending.leg2_filled_qty = update.filled_quantity;
            pending.leg2_avg_price = update.average_fill_price;
            pending.current_leg = 3;
            execute_next_leg(pending);
        } else if (update.is_terminal() && update.status != OrderStatus::FILLED) {
            // Leg 2 failed - need to reverse leg 1
            const auto* triangle = pending.opportunity.triangle;
            if (triangle && pending.leg1_filled) {
                Signal reverse_signal;
                reverse_signal.symbol = triangle->leg1_symbol;
                reverse_signal.exchange = params_.exchange;
                reverse_signal.type = pending.opportunity.use_forward ?
                    (triangle->leg1_is_buy ? SignalType::SELL : SignalType::BUY) :
                    (triangle->leg1_is_buy ? SignalType::BUY : SignalType::SELL);
                reverse_signal.target_quantity = pending.leg1_filled_qty;
                reverse_signal.reason = "Triangle leg 2 failed - reversing leg 1";
                reverse_signal.urgency = 1.0;
                emit_signal(std::move(reverse_signal));
            }
            pending_triangles_.erase(tri_it);
        }
    } else if (update.order_id == pending.leg3_order_id) {
        if (update.status == OrderStatus::FILLED) {
            pending.leg3_filled = true;
            pending.leg3_filled_qty = update.filled_quantity;
            pending.leg3_avg_price = update.average_fill_price;

            // Triangle complete - calculate profit
            const auto* triangle = pending.opportunity.triangle;
            if (triangle) {
                // Calculate actual profit
                double profit = 0.0;
                // Simplified profit calculation - actual would depend on currency
                // This tracks that we successfully completed the triangle

                // Update statistics
                Triangle* mutable_tri = const_cast<Triangle*>(triangle);
                mutable_tri->executions_count++;
                mutable_tri->total_profit += profit;
                if (mutable_tri->recent_profits.size() >= 100) {
                    mutable_tri->recent_profits.pop_front();
                }
                mutable_tri->recent_profits.push_back(profit);
            }

            // Clean up
            order_to_triangle_.erase(pending.leg1_order_id);
            order_to_triangle_.erase(pending.leg2_order_id);
            order_to_triangle_.erase(pending.leg3_order_id);
            pending_triangles_.erase(tri_it);
        } else if (update.is_terminal() && update.status != OrderStatus::FILLED) {
            // Leg 3 failed - we're stuck with an imbalanced position
            // This is a critical situation that needs attention
            const auto* triangle = pending.opportunity.triangle;
            if (triangle) {
                // Try to close out by reversing the first two legs
                Signal close_signal;
                close_signal.type = SignalType::CLOSE_LONG;
                close_signal.reason = "Triangle leg 3 failed - critical: need manual intervention";
                close_signal.urgency = 1.0;
                emit_signal(std::move(close_signal));
            }
            pending_triangles_.erase(tri_it);
        }
    }
}

void TriangularArbitrageStrategy::on_trade(const Trade& trade) {
    if (!is_running()) {
        return;
    }

    update_position(trade);
}

// ============================================================================
// Configuration
// ============================================================================

void TriangularArbitrageStrategy::add_triangle(const std::string& base,
                                                const std::string& quote1,
                                                const std::string& quote2) {
    Triangle triangle;
    triangle.id = base + "_" + quote1 + "_" + quote2;
    triangle.currency_a = base;
    triangle.currency_b = quote1;
    triangle.currency_c = quote2;

    // Determine the trading pairs for each leg
    // Leg 1: A -> B (e.g., BTC -> ETH)
    auto* pair1 = find_pair(base, quote1);
    if (pair1) {
        triangle.leg1_symbol = pair1->symbol;
        // If base is the quote currency of the pair, we buy; otherwise sell
        triangle.leg1_is_buy = (pair1->quote_currency == base);
    }

    // Leg 2: B -> C (e.g., ETH -> USDT)
    auto* pair2 = find_pair(quote1, quote2);
    if (pair2) {
        triangle.leg2_symbol = pair2->symbol;
        triangle.leg2_is_buy = (pair2->quote_currency == quote1);
    }

    // Leg 3: C -> A (e.g., USDT -> BTC)
    auto* pair3 = find_pair(quote2, base);
    if (pair3) {
        triangle.leg3_symbol = pair3->symbol;
        triangle.leg3_is_buy = (pair3->quote_currency == quote2);
    }

    triangles_.push_back(std::move(triangle));
}

// ============================================================================
// Analytics
// ============================================================================

std::vector<TriangularArbitrageStrategy::TriangleStats>
TriangularArbitrageStrategy::get_triangle_stats() const {
    std::vector<TriangleStats> stats;
    stats.reserve(triangles_.size());

    for (const auto& triangle : triangles_) {
        TriangleStats ts;
        ts.path_description = triangle.currency_a + " -> " +
                             triangle.currency_b + " -> " +
                             triangle.currency_c + " -> " +
                             triangle.currency_a;
        ts.opportunities_detected = triangle.opportunities_count;
        ts.trades_executed = triangle.executions_count;
        ts.total_profit = triangle.total_profit;

        if (!triangle.recent_profits.empty()) {
            double sum = 0.0;
            double max_p = -std::numeric_limits<double>::infinity();
            for (double p : triangle.recent_profits) {
                sum += p;
                max_p = std::max(max_p, p);
            }
            ts.mean_profit_bps = sum / triangle.recent_profits.size();
            ts.max_profit_bps = max_p;
        }

        if (triangle.opportunities_count > 0) {
            ts.success_rate = static_cast<double>(triangle.executions_count) /
                             triangle.opportunities_count;
        }

        stats.push_back(std::move(ts));
    }

    return stats;
}

double TriangularArbitrageStrategy::get_current_triangle_profit(
    const std::string& base,
    const std::string& quote1,
    const std::string& quote2) const
{
    // Find the triangle
    std::string id = base + "_" + quote1 + "_" + quote2;
    for (const auto& triangle : triangles_) {
        if (triangle.id == id) {
            double forward = calculate_rate_product(triangle, true);
            double reverse = calculate_rate_product(triangle, false);

            // Convert to profit in bps
            double forward_profit = (forward - 1.0) * 10000.0;
            double reverse_profit = (1.0 / reverse - 1.0) * 10000.0;

            return std::max(forward_profit, reverse_profit);
        }
    }
    return 0.0;
}

// ============================================================================
// Core Logic Implementation
// ============================================================================

void TriangularArbitrageStrategy::update_pair_data(const MarketData& data) {
    auto [base, quote] = parse_symbol(data.symbol);
    if (base.empty() || quote.empty()) {
        return;
    }

    auto& pair = pairs_[data.symbol];
    pair.symbol = data.symbol;
    pair.base_currency = base;
    pair.quote_currency = quote;
    pair.bid_price = data.bid_price;
    pair.ask_price = data.ask_price;
    pair.bid_size = data.bid_size;
    pair.ask_size = data.ask_size;
    pair.timestamp = data.local_timestamp;
}

std::optional<TriangularArbitrageStrategy::TriangleOpportunity>
TriangularArbitrageStrategy::calculate_opportunity(const Triangle& triangle) {
    // Get pairs for all legs
    auto it1 = pairs_.find(triangle.leg1_symbol);
    auto it2 = pairs_.find(triangle.leg2_symbol);
    auto it3 = pairs_.find(triangle.leg3_symbol);

    if (it1 == pairs_.end() || it2 == pairs_.end() || it3 == pairs_.end()) {
        return std::nullopt;
    }

    const auto& pair1 = it1->second;
    const auto& pair2 = it2->second;
    const auto& pair3 = it3->second;

    if (!pair1.is_valid() || !pair2.is_valid() || !pair3.is_valid()) {
        return std::nullopt;
    }

    TriangleOpportunity opp;
    opp.triangle = &triangle;
    opp.detected_at = current_timestamp();

    // Calculate forward path: A -> B -> C -> A
    // Each step has a fee
    double fee_multiplier = 1.0 - params_.fee_rate;

    // Forward path
    double forward_rate1, forward_rate2, forward_rate3;

    // Leg 1: A to B
    if (triangle.leg1_is_buy) {
        // We're buying the base, paying in quote (which is A)
        // Rate = 1 / ask (how much base we get per unit of A)
        forward_rate1 = (1.0 / pair1.ask_price) * fee_multiplier;
        opp.leg1_price = pair1.ask_price;
    } else {
        // We're selling the base (which is A) to get quote
        forward_rate1 = pair1.bid_price * fee_multiplier;
        opp.leg1_price = pair1.bid_price;
    }

    // Leg 2: B to C
    if (triangle.leg2_is_buy) {
        forward_rate2 = (1.0 / pair2.ask_price) * fee_multiplier;
        opp.leg2_price = pair2.ask_price;
    } else {
        forward_rate2 = pair2.bid_price * fee_multiplier;
        opp.leg2_price = pair2.bid_price;
    }

    // Leg 3: C to A
    if (triangle.leg3_is_buy) {
        forward_rate3 = (1.0 / pair3.ask_price) * fee_multiplier;
        opp.leg3_price = pair3.ask_price;
    } else {
        forward_rate3 = pair3.bid_price * fee_multiplier;
        opp.leg3_price = pair3.bid_price;
    }

    opp.forward_rate_product = forward_rate1 * forward_rate2 * forward_rate3;
    opp.forward_profit_bps = (opp.forward_rate_product - 1.0) * 10000.0;

    // Calculate reverse path if enabled
    if (params_.check_reverse_paths) {
        double reverse_rate1, reverse_rate2, reverse_rate3;

        // Leg 3: A to C (reverse of C to A)
        if (triangle.leg3_is_buy) {
            reverse_rate1 = pair3.bid_price * fee_multiplier;
        } else {
            reverse_rate1 = (1.0 / pair3.ask_price) * fee_multiplier;
        }

        // Leg 2: C to B (reverse of B to C)
        if (triangle.leg2_is_buy) {
            reverse_rate2 = pair2.bid_price * fee_multiplier;
        } else {
            reverse_rate2 = (1.0 / pair2.ask_price) * fee_multiplier;
        }

        // Leg 1: B to A (reverse of A to B)
        if (triangle.leg1_is_buy) {
            reverse_rate3 = pair1.bid_price * fee_multiplier;
        } else {
            reverse_rate3 = (1.0 / pair1.ask_price) * fee_multiplier;
        }

        opp.reverse_rate_product = reverse_rate1 * reverse_rate2 * reverse_rate3;
        opp.reverse_profit_bps = (opp.reverse_rate_product - 1.0) * 10000.0;
    }

    // Determine best direction
    if (opp.forward_profit_bps >= opp.reverse_profit_bps) {
        opp.use_forward = true;
        opp.best_profit_bps = opp.forward_profit_bps;
    } else {
        opp.use_forward = false;
        opp.best_profit_bps = opp.reverse_profit_bps;
    }

    // Calculate maximum size
    opp.max_size = calculate_max_size(triangle, opp.use_forward);

    // Calculate quantities for each leg
    opp.leg1_quantity = std::min(opp.max_size, params_.max_position);
    // Subsequent leg quantities depend on the rates
    opp.leg2_quantity = opp.leg1_quantity * (opp.use_forward ? forward_rate1 : (1.0 / forward_rate1));
    opp.leg3_quantity = opp.leg2_quantity * (opp.use_forward ? forward_rate2 : (1.0 / forward_rate2));

    // Calculate confidence
    opp.confidence = calculate_confidence(opp);

    // Update statistics
    if (opp.best_profit_bps >= params_.min_profit_bps) {
        const_cast<Triangle*>(&triangle)->opportunities_count++;
    }

    return opp;
}

double TriangularArbitrageStrategy::calculate_rate_product(
    const Triangle& triangle, bool forward) const
{
    auto it1 = pairs_.find(triangle.leg1_symbol);
    auto it2 = pairs_.find(triangle.leg2_symbol);
    auto it3 = pairs_.find(triangle.leg3_symbol);

    if (it1 == pairs_.end() || it2 == pairs_.end() || it3 == pairs_.end()) {
        return 1.0;  // No arbitrage
    }

    double fee_mult = 1.0 - params_.fee_rate;

    if (forward) {
        double r1 = triangle.leg1_is_buy ?
            (1.0 / it1->second.ask_price) : it1->second.bid_price;
        double r2 = triangle.leg2_is_buy ?
            (1.0 / it2->second.ask_price) : it2->second.bid_price;
        double r3 = triangle.leg3_is_buy ?
            (1.0 / it3->second.ask_price) : it3->second.bid_price;
        return r1 * r2 * r3 * fee_mult * fee_mult * fee_mult;
    } else {
        double r1 = triangle.leg3_is_buy ?
            it3->second.bid_price : (1.0 / it3->second.ask_price);
        double r2 = triangle.leg2_is_buy ?
            it2->second.bid_price : (1.0 / it2->second.ask_price);
        double r3 = triangle.leg1_is_buy ?
            it1->second.bid_price : (1.0 / it1->second.ask_price);
        return r1 * r2 * r3 * fee_mult * fee_mult * fee_mult;
    }
}

Quantity TriangularArbitrageStrategy::calculate_max_size(
    const Triangle& triangle, bool forward) const
{
    auto it1 = pairs_.find(triangle.leg1_symbol);
    auto it2 = pairs_.find(triangle.leg2_symbol);
    auto it3 = pairs_.find(triangle.leg3_symbol);

    if (it1 == pairs_.end() || it2 == pairs_.end() || it3 == pairs_.end()) {
        return 0.0;
    }

    Quantity size1, size2, size3;

    if (forward) {
        size1 = triangle.leg1_is_buy ? it1->second.ask_size : it1->second.bid_size;
        size2 = triangle.leg2_is_buy ? it2->second.ask_size : it2->second.bid_size;
        size3 = triangle.leg3_is_buy ? it3->second.ask_size : it3->second.bid_size;
    } else {
        size1 = triangle.leg3_is_buy ? it3->second.bid_size : it3->second.ask_size;
        size2 = triangle.leg2_is_buy ? it2->second.bid_size : it2->second.ask_size;
        size3 = triangle.leg1_is_buy ? it1->second.bid_size : it1->second.ask_size;
    }

    // Return minimum available across all legs
    return std::min({size1, size2, size3, params_.max_position});
}

void TriangularArbitrageStrategy::execute_triangle(const TriangleOpportunity& opp) {
    if (!opp.triangle || opp.max_size < params_.min_trade_size) {
        return;
    }

    uint64_t triangle_id = next_triangle_id_.fetch_add(1, std::memory_order_relaxed);

    PendingTriangle pending;
    pending.opportunity = opp;
    pending.start_time = current_timestamp();
    pending.current_leg = 1;

    // Generate order IDs
    pending.leg1_order_id = triangle_id * 3;
    pending.leg2_order_id = triangle_id * 3 + 1;
    pending.leg3_order_id = triangle_id * 3 + 2;

    // Map orders to triangle
    order_to_triangle_[pending.leg1_order_id] = triangle_id;
    order_to_triangle_[pending.leg2_order_id] = triangle_id;
    order_to_triangle_[pending.leg3_order_id] = triangle_id;

    pending_triangles_[triangle_id] = pending;

    // Execute first leg
    execute_next_leg(pending_triangles_[triangle_id]);
}

void TriangularArbitrageStrategy::execute_next_leg(PendingTriangle& pending) {
    const auto* triangle = pending.opportunity.triangle;
    if (!triangle) return;

    Signal signal;
    signal.exchange = params_.exchange;
    signal.confidence = pending.opportunity.confidence;
    signal.urgency = 1.0;

    double slippage_mult = 1.0 + (params_.slippage_tolerance_bps / 10000.0);

    switch (pending.current_leg) {
        case 1: {
            signal.symbol = triangle->leg1_symbol;
            bool is_buy = pending.opportunity.use_forward ?
                triangle->leg1_is_buy : !triangle->leg1_is_buy;
            signal.type = is_buy ? SignalType::BUY : SignalType::SELL;
            signal.target_price = pending.opportunity.leg1_price *
                (is_buy ? slippage_mult : (1.0 / slippage_mult));
            signal.target_quantity = pending.opportunity.leg1_quantity;
            signal.reason = "Triangular arbitrage leg 1: " + triangle->id;
            break;
        }
        case 2: {
            signal.symbol = triangle->leg2_symbol;
            bool is_buy = pending.opportunity.use_forward ?
                triangle->leg2_is_buy : !triangle->leg2_is_buy;
            signal.type = is_buy ? SignalType::BUY : SignalType::SELL;
            signal.target_price = pending.opportunity.leg2_price *
                (is_buy ? slippage_mult : (1.0 / slippage_mult));
            // Adjust quantity based on actual fill of leg 1
            signal.target_quantity = pending.leg1_filled_qty *
                pending.leg1_avg_price / pending.opportunity.leg2_price;
            signal.reason = "Triangular arbitrage leg 2: " + triangle->id;
            break;
        }
        case 3: {
            signal.symbol = triangle->leg3_symbol;
            bool is_buy = pending.opportunity.use_forward ?
                triangle->leg3_is_buy : !triangle->leg3_is_buy;
            signal.type = is_buy ? SignalType::BUY : SignalType::SELL;
            signal.target_price = pending.opportunity.leg3_price *
                (is_buy ? slippage_mult : (1.0 / slippage_mult));
            // Adjust quantity based on actual fill of leg 2
            signal.target_quantity = pending.leg2_filled_qty *
                pending.leg2_avg_price / pending.opportunity.leg3_price;
            signal.reason = "Triangular arbitrage leg 3: " + triangle->id;
            break;
        }
        default:
            return;
    }

    signal.timeout_us = params_.execution_timeout_us;
    emit_signal(std::move(signal));
}

double TriangularArbitrageStrategy::calculate_confidence(
    const TriangleOpportunity& opp) const
{
    double confidence = 1.0;

    // Reduce confidence if profit margin is thin
    if (opp.best_profit_bps < params_.min_profit_bps * 3) {
        confidence *= opp.best_profit_bps / (params_.min_profit_bps * 3);
    }

    // Reduce confidence based on size availability
    if (opp.max_size < params_.min_trade_size * 10) {
        confidence *= opp.max_size / (params_.min_trade_size * 10);
    }

    // Reduce confidence based on data freshness
    auto now = current_timestamp();
    if (opp.triangle) {
        auto it1 = pairs_.find(opp.triangle->leg1_symbol);
        auto it2 = pairs_.find(opp.triangle->leg2_symbol);
        auto it3 = pairs_.find(opp.triangle->leg3_symbol);

        if (it1 != pairs_.end() && it2 != pairs_.end() && it3 != pairs_.end()) {
            int64_t max_age = std::max({
                (now - it1->second.timestamp).count(),
                (now - it2->second.timestamp).count(),
                (now - it3->second.timestamp).count()
            });

            double freshness = 1.0 - (static_cast<double>(max_age) / params_.max_quote_age_ns);
            confidence *= std::max(0.0, freshness);
        }
    }

    return std::clamp(confidence, 0.0, 1.0);
}

bool TriangularArbitrageStrategy::has_fresh_data(const Triangle& triangle) const {
    auto now = current_timestamp();

    auto it1 = pairs_.find(triangle.leg1_symbol);
    auto it2 = pairs_.find(triangle.leg2_symbol);
    auto it3 = pairs_.find(triangle.leg3_symbol);

    if (it1 == pairs_.end() || it2 == pairs_.end() || it3 == pairs_.end()) {
        return false;
    }

    return (now - it1->second.timestamp).count() < params_.max_quote_age_ns &&
           (now - it2->second.timestamp).count() < params_.max_quote_age_ns &&
           (now - it3->second.timestamp).count() < params_.max_quote_age_ns;
}

std::string TriangularArbitrageStrategy::make_pair_key(
    const std::string& base, const std::string& quote) const
{
    return base + "_" + quote;
}

TriangularArbitrageStrategy::TradingPair*
TriangularArbitrageStrategy::find_pair(
    const std::string& currency1, const std::string& currency2)
{
    // Try direct mapping
    std::string key1 = make_pair_key(currency1, currency2);
    auto it = pair_to_symbol_.find(key1);
    if (it != pair_to_symbol_.end()) {
        auto pair_it = pairs_.find(it->second);
        if (pair_it != pairs_.end()) {
            return &pair_it->second;
        }
        // Create placeholder
        auto& pair = pairs_[it->second];
        pair.symbol = it->second;
        pair.base_currency = currency1;
        pair.quote_currency = currency2;
        return &pair;
    }

    // Try reverse mapping
    std::string key2 = make_pair_key(currency2, currency1);
    it = pair_to_symbol_.find(key2);
    if (it != pair_to_symbol_.end()) {
        auto pair_it = pairs_.find(it->second);
        if (pair_it != pairs_.end()) {
            pair_it->second.is_inverted = true;
            return &pair_it->second;
        }
        auto& pair = pairs_[it->second];
        pair.symbol = it->second;
        pair.base_currency = currency2;
        pair.quote_currency = currency1;
        pair.is_inverted = true;
        return &pair;
    }

    return nullptr;
}

std::pair<std::string, std::string>
TriangularArbitrageStrategy::parse_symbol(const std::string& symbol) const {
    // Common crypto pairs - try to extract base/quote
    static const std::vector<std::string> quote_currencies = {
        "USDT", "USDC", "BUSD", "USD", "BTC", "ETH", "BNB"
    };

    for (const auto& quote : quote_currencies) {
        if (symbol.size() > quote.size() &&
            symbol.substr(symbol.size() - quote.size()) == quote) {
            return {symbol.substr(0, symbol.size() - quote.size()), quote};
        }
    }

    // Fallback: assume 3-letter base, rest is quote
    if (symbol.size() >= 6) {
        return {symbol.substr(0, 3), symbol.substr(3)};
    }

    return {"", ""};
}

void TriangularArbitrageStrategy::check_pending_timeouts() {
    auto now = current_timestamp();
    std::vector<uint64_t> to_remove;

    for (auto& [id, pending] : pending_triangles_) {
        int64_t elapsed = (now - pending.start_time).count() / 1000;  // Convert to us
        if (elapsed > params_.execution_timeout_us) {
            // Timeout - need to unwind any filled legs
            const auto* triangle = pending.opportunity.triangle;
            if (!triangle) continue;

            if (pending.leg1_filled && !pending.leg2_filled) {
                // Only leg 1 filled - reverse it
                Signal reverse_signal;
                reverse_signal.symbol = triangle->leg1_symbol;
                reverse_signal.exchange = params_.exchange;
                bool was_buy = pending.opportunity.use_forward ?
                    triangle->leg1_is_buy : !triangle->leg1_is_buy;
                reverse_signal.type = was_buy ? SignalType::SELL : SignalType::BUY;
                reverse_signal.target_quantity = pending.leg1_filled_qty;
                reverse_signal.reason = "Triangle timeout - reversing leg 1";
                reverse_signal.urgency = 1.0;
                emit_signal(std::move(reverse_signal));
            } else if (pending.leg2_filled && !pending.leg3_filled) {
                // Legs 1 and 2 filled - more complex unwind needed
                // Would need to reverse both legs
                Signal close_signal;
                close_signal.type = SignalType::CLOSE_LONG;
                close_signal.reason = "Triangle timeout with 2 legs filled - manual intervention needed";
                close_signal.urgency = 1.0;
                emit_signal(std::move(close_signal));
            }

            to_remove.push_back(id);
        }
    }

    for (uint64_t id : to_remove) {
        auto& pending = pending_triangles_[id];
        order_to_triangle_.erase(pending.leg1_order_id);
        order_to_triangle_.erase(pending.leg2_order_id);
        order_to_triangle_.erase(pending.leg3_order_id);
        pending_triangles_.erase(id);
    }
}

}  // namespace strategies
}  // namespace hft
