/**
 * @file adaptive_market_maker.cpp
 * @brief Implementation of Adaptive Market Making Strategy
 */

#include "strategies/market_making/adaptive_market_maker.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace hft {
namespace strategies {
namespace market_making {

// ============================================================================
// Constructor
// ============================================================================

AdaptiveMarketMaker::AdaptiveMarketMaker(StrategyConfig config, AdaptiveMMConfig mm_config)
    : BasicMarketMaker(std::move(config), mm_config)
    , adaptive_config_(std::move(mm_config))
{
    // Copy config to base class
    mm_config_ = adaptive_config_;
}

// ============================================================================
// Lifecycle Methods
// ============================================================================

bool AdaptiveMarketMaker::initialize() {
    if (!BasicMarketMaker::initialize()) {
        return false;
    }

    // Initialize volatility state
    volatility_stats_ = VolatilityStats{};
    last_ob_analysis_ = OrderBookAnalysis{};
    adaptive_params_ = AdaptiveParams{};

    // Clear history
    price_history_.clear();
    high_low_bars_.clear();
    fill_history_.clear();
    volume_history_.clear();

    // Initialize current bar
    current_bar_ = HighLowBar{};

    // Initialize EWMA variance
    ewma_variance_ = 0.0;
    last_price_for_vol_ = 0.0;

    return true;
}

void AdaptiveMarketMaker::reset() {
    BasicMarketMaker::reset();
    on_reset();
}

void AdaptiveMarketMaker::on_reset() {
    volatility_stats_ = VolatilityStats{};
    last_ob_analysis_ = OrderBookAnalysis{};
    adaptive_params_ = AdaptiveParams{};
    price_history_.clear();
    high_low_bars_.clear();
    fill_history_.clear();
    volume_history_.clear();
    current_bar_ = HighLowBar{};
    ewma_variance_ = 0.0;
    last_price_for_vol_ = 0.0;
    baseline_volume_ = 0.0;
}

// ============================================================================
// Market Data Handler
// ============================================================================

void AdaptiveMarketMaker::on_market_data(const MarketData& data) {
    if (!is_running()) {
        return;
    }

    // Validate market data
    if (data.bid_price <= 0 || data.ask_price <= 0) {
        return;
    }

    Price mid_price = data.mid_price();

    // Update volatility estimate
    update_volatility(mid_price, data.timestamp);

    // Analyze order book
    last_ob_analysis_ = analyze_orderbook(data.orderbook);

    // Update volume tracking
    VolumeRecord vol_record{data.timestamp, data.last_size};
    volume_history_.push_back(vol_record);
    while (!volume_history_.empty() &&
           (data.timestamp - volume_history_.front().time).count() >
           static_cast<int64_t>(adaptive_config_.volume_lookback_seconds * 1e9)) {
        volume_history_.pop_front();
    }

    // Update adaptive parameters
    update_adaptive_params();

    // Call base class handler (will use our overridden calculate_quotes)
    BasicMarketMaker::on_market_data(data);
}

// ============================================================================
// Order Event Handlers
// ============================================================================

void AdaptiveMarketMaker::on_order_update(const OrderUpdate& update) {
    BasicMarketMaker::on_order_update(update);

    // Track fill for fill rate calculation
    if (update.is_terminal()) {
        FillRecord record;
        record.time = update.update_timestamp;
        record.was_filled = (update.status == OrderStatus::FILLED ||
                            update.status == OrderStatus::PARTIALLY_FILLED);
        fill_history_.push_back(record);

        // Trim old records
        auto cutoff = current_timestamp();
        while (!fill_history_.empty() &&
               (cutoff - fill_history_.front().time).count() >
               static_cast<int64_t>(adaptive_config_.fill_rate_lookback_seconds * 1e9)) {
            fill_history_.pop_front();
        }
    }
}

void AdaptiveMarketMaker::on_trade(const Trade& trade) {
    BasicMarketMaker::on_trade(trade);
}

// ============================================================================
// Volatility Estimation
// ============================================================================

void AdaptiveMarketMaker::update_volatility(Price price, Timestamp timestamp) {
    // Store price sample
    PriceSample sample;
    sample.price = price;
    sample.timestamp = timestamp;

    if (!price_history_.empty()) {
        // Calculate log return
        double log_return = std::log(price / price_history_.back().price);
        sample.return_value = log_return;

        // Update EWMA volatility
        volatility_stats_.ewma_volatility = calculate_ewma_volatility(log_return);
    }

    price_history_.push_back(sample);

    // Trim old samples
    while (!price_history_.empty() &&
           (timestamp - price_history_.front().timestamp).count() >
           static_cast<int64_t>(adaptive_config_.volatility_lookback_seconds * 1e9)) {
        price_history_.pop_front();
    }

    // Update high-low bar for Parkinson volatility
    int64_t bar_start_ms = (timestamp.count() / 1000000) / BAR_DURATION_MS * BAR_DURATION_MS;
    Timestamp bar_start{std::chrono::milliseconds(bar_start_ms)};

    if (current_bar_.start_time.count() == 0 || bar_start > current_bar_.start_time) {
        // New bar
        if (current_bar_.start_time.count() > 0) {
            current_bar_.close = price_history_.empty() ? price :
                                 price_history_.back().price;
            high_low_bars_.push_back(current_bar_);

            // Trim old bars
            while (high_low_bars_.size() > 100) {
                high_low_bars_.pop_front();
            }
        }

        current_bar_ = HighLowBar{};
        current_bar_.start_time = bar_start;
        current_bar_.open = price;
        current_bar_.high = price;
        current_bar_.low = price;
    } else {
        current_bar_.high = std::max(current_bar_.high, price);
        current_bar_.low = std::min(current_bar_.low, price);
    }

    // Calculate realized volatility
    volatility_stats_.realized_volatility = calculate_realized_volatility();

    // Calculate Parkinson volatility
    volatility_stats_.high_low_volatility = calculate_parkinson_volatility();

    // Select current volatility based on method
    switch (adaptive_config_.volatility_method) {
        case VolatilityMethod::EWMA:
            volatility_stats_.current_volatility = volatility_stats_.ewma_volatility;
            break;
        case VolatilityMethod::PARKINSON:
            volatility_stats_.current_volatility = volatility_stats_.high_low_volatility;
            break;
        case VolatilityMethod::REALIZED:
            volatility_stats_.current_volatility = volatility_stats_.realized_volatility;
            break;
        default:
            volatility_stats_.current_volatility = volatility_stats_.ewma_volatility;
    }

    // Clamp volatility
    volatility_stats_.current_volatility = std::clamp(
        volatility_stats_.current_volatility,
        adaptive_config_.min_volatility_estimate,
        adaptive_config_.max_volatility_estimate
    );

    volatility_stats_.last_update = timestamp;
    last_price_for_vol_ = price;
}

double AdaptiveMarketMaker::calculate_ewma_volatility() const {
    return std::sqrt(ewma_variance_);
}

double AdaptiveMarketMaker::calculate_ewma_volatility(double new_return) {
    double decay = adaptive_config_.volatility_decay_factor;
    double squared_return = new_return * new_return;

    // EWMA update: variance_t = decay * variance_{t-1} + (1-decay) * return^2
    ewma_variance_ = decay * ewma_variance_ + (1.0 - decay) * squared_return;

    return std::sqrt(ewma_variance_);
}

double AdaptiveMarketMaker::calculate_parkinson_volatility() const {
    /**
     * Parkinson volatility estimator:
     * sigma^2 = (1/4*ln(2)) * (1/n) * sum(ln(H/L)^2)
     *
     * This estimator uses high-low range and is more efficient than
     * close-to-close volatility when prices follow continuous diffusion.
     */
    if (high_low_bars_.empty()) {
        return adaptive_config_.min_volatility_estimate;
    }

    double sum_squared = 0.0;
    int count = 0;

    for (const auto& bar : high_low_bars_) {
        if (bar.low > 0 && bar.high > bar.low) {
            double log_range = std::log(bar.high / bar.low);
            sum_squared += log_range * log_range;
            count++;
        }
    }

    if (count == 0) {
        return adaptive_config_.min_volatility_estimate;
    }

    // Parkinson constant: 1 / (4 * ln(2)) = 0.3607
    constexpr double PARKINSON_CONST = 0.3607;
    double variance = PARKINSON_CONST * sum_squared / count;

    // Annualize (assuming BAR_DURATION_MS bars)
    double bars_per_year = (365.25 * 24.0 * 3600.0 * 1000.0) / BAR_DURATION_MS;
    double annualized_vol = std::sqrt(variance * bars_per_year);

    return annualized_vol;
}

double AdaptiveMarketMaker::calculate_realized_volatility() const {
    /**
     * Simple realized volatility:
     * sigma = sqrt(sum(r^2) / n) * sqrt(periods_per_year)
     */
    if (price_history_.size() < 2) {
        return adaptive_config_.min_volatility_estimate;
    }

    double sum_squared = 0.0;
    int count = 0;

    for (const auto& sample : price_history_) {
        if (sample.return_value != 0.0) {
            sum_squared += sample.return_value * sample.return_value;
            count++;
        }
    }

    if (count == 0) {
        return adaptive_config_.min_volatility_estimate;
    }

    double variance = sum_squared / count;

    // Estimate sampling frequency and annualize
    if (price_history_.size() >= 2) {
        auto time_span = price_history_.back().timestamp - price_history_.front().timestamp;
        double seconds = time_span.count() / 1e9;
        if (seconds > 0) {
            double samples_per_second = count / seconds;
            double samples_per_year = samples_per_second * 365.25 * 24.0 * 3600.0;
            return std::sqrt(variance * samples_per_year);
        }
    }

    return std::sqrt(variance);
}

// ============================================================================
// Order Book Analysis
// ============================================================================

OrderBookAnalysis AdaptiveMarketMaker::analyze_orderbook(const OrderBookSnapshot& orderbook) const {
    OrderBookAnalysis analysis;

    // Calculate bid depth
    for (size_t i = 0; i < orderbook.bid_levels && i < orderbook.MAX_LEVELS; ++i) {
        if (orderbook.bids[i].is_valid()) {
            analysis.bid_depth += orderbook.bids[i].quantity;
            analysis.bid_levels++;
        }
    }

    // Calculate ask depth
    for (size_t i = 0; i < orderbook.ask_levels && i < orderbook.MAX_LEVELS; ++i) {
        if (orderbook.asks[i].is_valid()) {
            analysis.ask_depth += orderbook.asks[i].quantity;
            analysis.ask_levels++;
        }
    }

    // Calculate imbalance
    double total_depth = analysis.bid_depth + analysis.ask_depth;
    if (total_depth > 0) {
        analysis.imbalance = (analysis.bid_depth - analysis.ask_depth) / total_depth;
    }

    // Calculate volume-weighted mid price
    if (orderbook.bid_levels > 0 && orderbook.ask_levels > 0) {
        double bid_value = orderbook.bids[0].price * orderbook.bids[0].quantity;
        double ask_value = orderbook.asks[0].price * orderbook.asks[0].quantity;
        double total_value = bid_value + ask_value;
        if (total_value > 0) {
            analysis.weighted_mid_price = (bid_value * orderbook.asks[0].price +
                                          ask_value * orderbook.bids[0].price) / total_value;
        }
    }

    // Calculate microprice
    analysis.microprice = calculate_microprice(orderbook);

    return analysis;
}

Price AdaptiveMarketMaker::calculate_microprice(const OrderBookSnapshot& orderbook) const {
    /**
     * Microprice formula:
     * microprice = (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)
     *
     * This is the imbalance-weighted mid price, which better reflects
     * where the next trade is likely to occur.
     */
    if (orderbook.bid_levels == 0 || orderbook.ask_levels == 0) {
        return orderbook.mid_price();
    }

    double bid = orderbook.bids[0].price;
    double ask = orderbook.asks[0].price;
    double bid_qty = orderbook.bids[0].quantity;
    double ask_qty = orderbook.asks[0].quantity;

    if (bid_qty + ask_qty <= 0) {
        return (bid + ask) / 2.0;
    }

    return (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty);
}

// ============================================================================
// Adaptive Parameter Calculation
// ============================================================================

double AdaptiveMarketMaker::compute_adaptive_spread() const {
    double base_spread = adaptive_config_.target_spread_bps;

    // 1. Volatility adjustment
    // Higher volatility = wider spread
    double vol = volatility_stats_.current_volatility;
    double vol_adjustment = 1.0 + adaptive_config_.spread_volatility_multiplier * vol;

    // 2. Imbalance adjustment
    // Asymmetric imbalance widens spread on the imbalanced side
    double imb = std::abs(last_ob_analysis_.imbalance);
    double imb_adjustment = 1.0 + adaptive_config_.spread_imbalance_multiplier * imb;

    // 3. Fill rate optimization
    double fill_adjustment = optimize_for_fill_rate();

    // 4. Momentum adjustment
    double momentum_adjustment = 1.0;
    if (adaptive_config_.enable_momentum_filter) {
        double momentum = std::abs(calculate_momentum());
        if (momentum > adaptive_config_.momentum_threshold) {
            momentum_adjustment = adaptive_config_.momentum_spread_multiplier;
        }
    }

    // Combine adjustments
    double adjusted_spread = base_spread * vol_adjustment * imb_adjustment *
                            fill_adjustment * momentum_adjustment;

    // Clamp to limits
    return std::clamp(adjusted_spread, adaptive_config_.min_spread_bps,
                     adaptive_config_.max_spread_bps);
}

std::pair<Quantity, Quantity> AdaptiveMarketMaker::compute_adaptive_sizes() const {
    double base_size = adaptive_config_.base_order_size;

    // 1. Volume-based adjustment
    double volume_ratio = 1.0;
    if (!volume_history_.empty() && baseline_volume_ > 0) {
        double recent_volume = 0.0;
        for (const auto& v : volume_history_) {
            recent_volume += v.volume;
        }
        volume_ratio = recent_volume / baseline_volume_;
        volume_ratio = std::clamp(volume_ratio, adaptive_config_.min_volume_ratio,
                                 adaptive_config_.max_volume_ratio);
    }

    double volume_adjusted = base_size * volume_ratio * adaptive_config_.volume_size_multiplier;

    // 2. Imbalance-based asymmetric sizing
    double bid_size = volume_adjusted;
    double ask_size = volume_adjusted;

    if (std::abs(last_ob_analysis_.imbalance) > adaptive_config_.imbalance_threshold) {
        if (last_ob_analysis_.imbalance > 0) {
            // More bids than asks - reduce bid size, increase ask size
            bid_size *= (1.0 - adaptive_config_.imbalance_skew_factor);
            ask_size *= (1.0 + adaptive_config_.imbalance_skew_factor);
        } else {
            // More asks than bids
            bid_size *= (1.0 + adaptive_config_.imbalance_skew_factor);
            ask_size *= (1.0 - adaptive_config_.imbalance_skew_factor);
        }
    }

    // 3. Volatility-based reduction in high vol
    if (volatility_stats_.current_volatility > adaptive_config_.high_volatility_threshold) {
        double vol_reduction = std::min(0.5, volatility_stats_.current_volatility /
                                        adaptive_config_.high_volatility_threshold - 1.0);
        bid_size *= (1.0 - vol_reduction);
        ask_size *= (1.0 - vol_reduction);
    }

    // Apply limits
    bid_size = std::clamp(bid_size, adaptive_config_.min_order_size,
                         adaptive_config_.max_order_size);
    ask_size = std::clamp(ask_size, adaptive_config_.min_order_size,
                         adaptive_config_.max_order_size);

    return {round_quantity(bid_size), round_quantity(ask_size)};
}

MarketRegime AdaptiveMarketMaker::detect_regime() const {
    if (!adaptive_config_.enable_regime_detection) {
        return MarketRegime::NORMAL;
    }

    double vol = volatility_stats_.current_volatility;
    double momentum = calculate_momentum();

    // Volatility-based classification
    if (vol < adaptive_config_.low_volatility_threshold) {
        return MarketRegime::LOW_VOLATILITY;
    } else if (vol > adaptive_config_.high_volatility_threshold) {
        return MarketRegime::HIGH_VOLATILITY;
    }

    // Momentum-based classification
    if (std::abs(momentum) > adaptive_config_.momentum_threshold) {
        if (momentum > 0) {
            return MarketRegime::TRENDING_UP;
        } else {
            return MarketRegime::TRENDING_DOWN;
        }
    }

    return MarketRegime::NORMAL;
}

double AdaptiveMarketMaker::calculate_momentum() const {
    if (price_history_.size() < 2) {
        return 0.0;
    }

    // Calculate momentum as price change over lookback period
    auto now = current_timestamp();
    Price current_price = price_history_.back().price;
    Price lookback_price = current_price;

    for (auto it = price_history_.rbegin(); it != price_history_.rend(); ++it) {
        if ((now - it->timestamp).count() >=
            static_cast<int64_t>(adaptive_config_.momentum_lookback_seconds * 1e9)) {
            lookback_price = it->price;
            break;
        }
    }

    if (lookback_price <= 0) {
        return 0.0;
    }

    return (current_price - lookback_price) / lookback_price;
}

void AdaptiveMarketMaker::update_adaptive_params() {
    adaptive_params_.adjusted_spread_bps = compute_adaptive_spread();
    auto [bid_size, ask_size] = compute_adaptive_sizes();
    adaptive_params_.adjusted_bid_size = bid_size;
    adaptive_params_.adjusted_ask_size = ask_size;
    adaptive_params_.current_regime = detect_regime();
    adaptive_params_.last_computation = current_timestamp();

    // Calculate quote offset based on microprice
    if (last_ob_analysis_.microprice > 0 && last_mid_price_ > 0) {
        adaptive_params_.quote_offset = (last_ob_analysis_.microprice - last_mid_price_) /
                                       last_mid_price_;
    }
}

// ============================================================================
// Quote Calculation Override
// ============================================================================

std::pair<Price, Price> AdaptiveMarketMaker::calculate_quotes(Price mid_price) const {
    // Use adaptive spread
    double spread_bps = adaptive_params_.adjusted_spread_bps;
    double half_spread = (spread_bps / 10000.0) * mid_price / 2.0;

    // Use microprice if available
    Price reference_price = mid_price;
    if (adaptive_config_.use_microprice && last_ob_analysis_.microprice > 0) {
        reference_price = last_ob_analysis_.microprice;
    }

    // Calculate inventory skew (from base class)
    double skew = BasicMarketMaker::calculate_inventory_skew();
    double skew_amount = skew * half_spread * mm_config_.inventory_skew_factor;

    // Calculate prices
    Price bid_price = reference_price - half_spread + skew_amount;
    Price ask_price = reference_price + half_spread + skew_amount;

    // Round to tick size
    bid_price = round_price(bid_price);
    ask_price = round_price(ask_price);

    // Ensure proper spread
    if (bid_price >= ask_price) {
        ask_price = bid_price + mm_config_.tick_size;
    }

    return {bid_price, ask_price};
}

Quantity AdaptiveMarketMaker::calculate_order_size(OrderSide side) const {
    // Use adaptive sizes
    if (side == OrderSide::BUY) {
        return adaptive_params_.adjusted_bid_size;
    } else {
        return adaptive_params_.adjusted_ask_size;
    }
}

// ============================================================================
// Fill Rate Optimization
// ============================================================================

double AdaptiveMarketMaker::calculate_fill_rate() const {
    if (fill_history_.empty()) {
        return 0.5;  // Default
    }

    int filled = 0;
    for (const auto& record : fill_history_) {
        if (record.was_filled) {
            filled++;
        }
    }

    return static_cast<double>(filled) / fill_history_.size();
}

double AdaptiveMarketMaker::optimize_for_fill_rate() const {
    double current_rate = calculate_fill_rate();
    double target_rate = adaptive_config_.target_fill_rate;

    // If fill rate too low, tighten spread
    // If fill rate too high, widen spread (might be getting picked off)
    double adjustment = 1.0;

    if (current_rate < target_rate * 0.8) {
        // Fill rate too low - tighten spread
        adjustment = 1.0 - adaptive_config_.fill_rate_adjustment_factor;
    } else if (current_rate > target_rate * 1.2) {
        // Fill rate too high - might be adverse selection
        adjustment = 1.0 + adaptive_config_.fill_rate_adjustment_factor;
    }

    return adjustment;
}

}  // namespace market_making
}  // namespace strategies
}  // namespace hft
