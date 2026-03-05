#pragma once

/**
 * @file math_utils.hpp
 * @brief Mathematical utilities for HFT operations
 *
 * This module provides high-performance mathematical utilities optimized for HFT.
 * Features:
 * - Fixed-point arithmetic for prices
 * - Statistical functions (mean, std, variance, correlation)
 * - Rolling window calculations
 * - Exponential moving average
 * - Percentile calculations
 *
 * @author HFT System
 * @version 1.0
 */

#include <cstdint>
#include <cmath>
#include <vector>
#include <deque>
#include <algorithm>
#include <numeric>
#include <optional>
#include <limits>
#include <stdexcept>
#include <array>
#include <type_traits>

namespace hft {
namespace utils {

// ============================================================================
// Fixed-Point Arithmetic
// ============================================================================

/**
 * @brief Fixed-point decimal for precise price calculations
 *
 * Uses 64-bit integer internally with configurable decimal places.
 * Default is 8 decimal places (satoshi precision).
 */
template<int Decimals = 8>
class FixedPoint {
public:
    static constexpr int64_t SCALE = []() constexpr {
        int64_t s = 1;
        for (int i = 0; i < Decimals; ++i) s *= 10;
        return s;
    }();

    constexpr FixedPoint() : value_(0) {}

    constexpr explicit FixedPoint(int64_t raw_value, bool raw) : value_(raw_value) {
        (void)raw;  // tag parameter
    }

    constexpr FixedPoint(double value)
        : value_(static_cast<int64_t>(std::round(value * SCALE))) {}

    constexpr FixedPoint(int value)
        : value_(static_cast<int64_t>(value) * SCALE) {}

    constexpr FixedPoint(int64_t value)
        : value_(value * SCALE) {}

    // Conversion
    constexpr double to_double() const { return static_cast<double>(value_) / SCALE; }
    constexpr int64_t raw() const { return value_; }

    static constexpr FixedPoint from_raw(int64_t raw) { return FixedPoint(raw, true); }

    // Arithmetic operations
    constexpr FixedPoint operator+(const FixedPoint& other) const {
        return FixedPoint(value_ + other.value_, true);
    }

    constexpr FixedPoint operator-(const FixedPoint& other) const {
        return FixedPoint(value_ - other.value_, true);
    }

    constexpr FixedPoint operator*(const FixedPoint& other) const {
        // Use 128-bit intermediate to prevent overflow
        __int128 result = static_cast<__int128>(value_) * other.value_ / SCALE;
        return FixedPoint(static_cast<int64_t>(result), true);
    }

    constexpr FixedPoint operator/(const FixedPoint& other) const {
        if (other.value_ == 0) {
            return FixedPoint(0, true);
        }
        __int128 result = static_cast<__int128>(value_) * SCALE / other.value_;
        return FixedPoint(static_cast<int64_t>(result), true);
    }

    constexpr FixedPoint operator*(double scalar) const {
        return FixedPoint(static_cast<int64_t>(value_ * scalar), true);
    }

    constexpr FixedPoint operator/(double scalar) const {
        return FixedPoint(static_cast<int64_t>(value_ / scalar), true);
    }

    // Compound assignment
    constexpr FixedPoint& operator+=(const FixedPoint& other) {
        value_ += other.value_;
        return *this;
    }

    constexpr FixedPoint& operator-=(const FixedPoint& other) {
        value_ -= other.value_;
        return *this;
    }

    // Comparison
    constexpr bool operator==(const FixedPoint& other) const { return value_ == other.value_; }
    constexpr bool operator!=(const FixedPoint& other) const { return value_ != other.value_; }
    constexpr bool operator<(const FixedPoint& other) const { return value_ < other.value_; }
    constexpr bool operator<=(const FixedPoint& other) const { return value_ <= other.value_; }
    constexpr bool operator>(const FixedPoint& other) const { return value_ > other.value_; }
    constexpr bool operator>=(const FixedPoint& other) const { return value_ >= other.value_; }

    // Unary
    constexpr FixedPoint operator-() const { return FixedPoint(-value_, true); }
    constexpr FixedPoint abs() const { return FixedPoint(value_ >= 0 ? value_ : -value_, true); }

    // Check for zero
    constexpr bool is_zero() const { return value_ == 0; }
    constexpr bool is_positive() const { return value_ > 0; }
    constexpr bool is_negative() const { return value_ < 0; }

    // Round to tick size
    FixedPoint round_to_tick(const FixedPoint& tick_size) const {
        if (tick_size.value_ == 0) return *this;
        int64_t ticks = (value_ + tick_size.value_ / 2) / tick_size.value_;
        return FixedPoint(ticks * tick_size.value_, true);
    }

    // Floor to tick size
    FixedPoint floor_to_tick(const FixedPoint& tick_size) const {
        if (tick_size.value_ == 0) return *this;
        int64_t ticks = value_ / tick_size.value_;
        return FixedPoint(ticks * tick_size.value_, true);
    }

    // Ceil to tick size
    FixedPoint ceil_to_tick(const FixedPoint& tick_size) const {
        if (tick_size.value_ == 0) return *this;
        int64_t ticks = (value_ + tick_size.value_ - 1) / tick_size.value_;
        return FixedPoint(ticks * tick_size.value_, true);
    }

private:
    int64_t value_;
};

// Common fixed-point types
using Price = FixedPoint<8>;     // 8 decimal places (satoshi precision)
using Quantity = FixedPoint<8>;   // 8 decimal places
using Percent = FixedPoint<6>;    // 6 decimal places (0.000001 = 0.0001%)

// ============================================================================
// Statistical Functions
// ============================================================================

/**
 * @brief Calculate arithmetic mean
 */
template<typename Container>
double mean(const Container& data) {
    if (data.empty()) return 0.0;
    double sum = std::accumulate(data.begin(), data.end(), 0.0);
    return sum / data.size();
}

/**
 * @brief Calculate variance
 */
template<typename Container>
double variance(const Container& data, bool sample = true) {
    if (data.size() < 2) return 0.0;
    double m = mean(data);
    double sum_sq = 0.0;
    for (const auto& x : data) {
        double diff = x - m;
        sum_sq += diff * diff;
    }
    return sum_sq / (sample ? data.size() - 1 : data.size());
}

/**
 * @brief Calculate standard deviation
 */
template<typename Container>
double stddev(const Container& data, bool sample = true) {
    return std::sqrt(variance(data, sample));
}

/**
 * @brief Calculate covariance
 */
template<typename Container>
double covariance(const Container& x, const Container& y, bool sample = true) {
    if (x.size() != y.size() || x.size() < 2) return 0.0;

    double mean_x = mean(x);
    double mean_y = mean(y);

    double sum = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        sum += (x[i] - mean_x) * (y[i] - mean_y);
    }

    return sum / (sample ? x.size() - 1 : x.size());
}

/**
 * @brief Calculate Pearson correlation coefficient
 */
template<typename Container>
double correlation(const Container& x, const Container& y) {
    if (x.size() != y.size() || x.size() < 2) return 0.0;

    double std_x = stddev(x);
    double std_y = stddev(y);

    if (std_x == 0.0 || std_y == 0.0) return 0.0;

    return covariance(x, y) / (std_x * std_y);
}

/**
 * @brief Calculate skewness
 */
template<typename Container>
double skewness(const Container& data) {
    if (data.size() < 3) return 0.0;

    double m = mean(data);
    double s = stddev(data);
    if (s == 0.0) return 0.0;

    double sum = 0.0;
    for (const auto& x : data) {
        double z = (x - m) / s;
        sum += z * z * z;
    }

    return sum * data.size() / ((data.size() - 1.0) * (data.size() - 2.0));
}

/**
 * @brief Calculate kurtosis (excess)
 */
template<typename Container>
double kurtosis(const Container& data) {
    if (data.size() < 4) return 0.0;

    double m = mean(data);
    double s = stddev(data);
    if (s == 0.0) return 0.0;

    double sum = 0.0;
    for (const auto& x : data) {
        double z = (x - m) / s;
        sum += z * z * z * z;
    }

    double n = static_cast<double>(data.size());
    return (n * (n + 1.0) * sum / ((n - 1.0) * (n - 2.0) * (n - 3.0)))
           - 3.0 * (n - 1.0) * (n - 1.0) / ((n - 2.0) * (n - 3.0));
}

/**
 * @brief Calculate percentile
 */
template<typename Container>
double percentile(const Container& data, double p) {
    if (data.empty()) return 0.0;
    if (p <= 0.0) return *std::min_element(data.begin(), data.end());
    if (p >= 100.0) return *std::max_element(data.begin(), data.end());

    std::vector<typename Container::value_type> sorted(data.begin(), data.end());
    std::sort(sorted.begin(), sorted.end());

    double index = (p / 100.0) * (sorted.size() - 1);
    size_t lower = static_cast<size_t>(std::floor(index));
    size_t upper = static_cast<size_t>(std::ceil(index));

    if (lower == upper) return sorted[lower];

    double frac = index - lower;
    return sorted[lower] * (1.0 - frac) + sorted[upper] * frac;
}

/**
 * @brief Calculate median
 */
template<typename Container>
double median(const Container& data) {
    return percentile(data, 50.0);
}

/**
 * @brief Calculate z-score
 */
inline double zscore(double value, double mean, double stddev) {
    if (stddev == 0.0) return 0.0;
    return (value - mean) / stddev;
}

// ============================================================================
// Rolling Window Calculator
// ============================================================================

/**
 * @brief Rolling window statistics calculator
 *
 * Provides O(1) updates for mean and variance using Welford's algorithm
 */
class RollingStats {
public:
    explicit RollingStats(size_t window_size)
        : window_size_(window_size), n_(0), mean_(0.0), m2_(0.0) {}

    /**
     * @brief Add a new value to the window
     */
    void push(double value) {
        if (values_.size() >= window_size_) {
            // Remove oldest value
            double old_value = values_.front();
            values_.pop_front();

            // Update stats (reverse Welford for removal)
            double old_mean = mean_;
            mean_ = (mean_ * (n_) - old_value) / (n_ - 1);
            m2_ -= (old_value - old_mean) * (old_value - mean_);
            --n_;
        }

        // Add new value (Welford's online algorithm)
        values_.push_back(value);
        ++n_;
        double delta = value - mean_;
        mean_ += delta / n_;
        double delta2 = value - mean_;
        m2_ += delta * delta2;
    }

    /**
     * @brief Clear all data
     */
    void clear() {
        values_.clear();
        n_ = 0;
        mean_ = 0.0;
        m2_ = 0.0;
    }

    /**
     * @brief Get current count
     */
    size_t count() const { return n_; }

    /**
     * @brief Check if window is full
     */
    bool is_full() const { return n_ >= window_size_; }

    /**
     * @brief Get current mean
     */
    double get_mean() const { return mean_; }

    /**
     * @brief Get current variance
     */
    double get_variance(bool sample = true) const {
        if (n_ < 2) return 0.0;
        return m2_ / (sample ? n_ - 1 : n_);
    }

    /**
     * @brief Get current standard deviation
     */
    double get_stddev(bool sample = true) const {
        return std::sqrt(get_variance(sample));
    }

    /**
     * @brief Get z-score for a value
     */
    double get_zscore(double value) const {
        double s = get_stddev();
        if (s == 0.0) return 0.0;
        return (value - mean_) / s;
    }

    /**
     * @brief Get minimum value in window
     */
    double get_min() const {
        if (values_.empty()) return 0.0;
        return *std::min_element(values_.begin(), values_.end());
    }

    /**
     * @brief Get maximum value in window
     */
    double get_max() const {
        if (values_.empty()) return 0.0;
        return *std::max_element(values_.begin(), values_.end());
    }

    /**
     * @brief Get the underlying values
     */
    const std::deque<double>& values() const { return values_; }

private:
    size_t window_size_;
    size_t n_;
    double mean_;
    double m2_;
    std::deque<double> values_;
};

// ============================================================================
// Exponential Moving Average
// ============================================================================

/**
 * @brief Exponential Moving Average calculator
 */
class EMA {
public:
    /**
     * @brief Construct EMA with span
     * @param span Number of periods for decay (like pandas)
     */
    explicit EMA(double span) : initialized_(false), value_(0.0) {
        alpha_ = 2.0 / (span + 1.0);
    }

    /**
     * @brief Construct EMA with alpha directly
     * @param alpha Smoothing factor (0 < alpha <= 1)
     * @param use_alpha Tag to indicate alpha is provided directly
     */
    EMA(double alpha, bool use_alpha) : alpha_(alpha), initialized_(false), value_(0.0) {
        (void)use_alpha;
    }

    /**
     * @brief Update with new value
     * @return Updated EMA value
     */
    double update(double value) {
        if (!initialized_) {
            value_ = value;
            initialized_ = true;
        } else {
            value_ = alpha_ * value + (1.0 - alpha_) * value_;
        }
        return value_;
    }

    /**
     * @brief Get current EMA value
     */
    double value() const { return value_; }

    /**
     * @brief Reset EMA
     */
    void reset() {
        initialized_ = false;
        value_ = 0.0;
    }

    /**
     * @brief Check if initialized
     */
    bool is_initialized() const { return initialized_; }

    /**
     * @brief Get alpha value
     */
    double alpha() const { return alpha_; }

private:
    double alpha_;
    bool initialized_;
    double value_;
};

/**
 * @brief Double Exponential Moving Average (DEMA)
 */
class DEMA {
public:
    explicit DEMA(double span) : ema1_(span), ema2_(span) {}

    double update(double value) {
        double e1 = ema1_.update(value);
        double e2 = ema2_.update(e1);
        return 2.0 * e1 - e2;
    }

    double value() const {
        return 2.0 * ema1_.value() - ema2_.value();
    }

    void reset() {
        ema1_.reset();
        ema2_.reset();
    }

private:
    EMA ema1_;
    EMA ema2_;
};

/**
 * @brief Exponentially Weighted Moving Variance
 */
class EWMV {
public:
    explicit EWMV(double span)
        : alpha_(2.0 / (span + 1.0)), initialized_(false),
          mean_(0.0), var_(0.0) {}

    double update(double value) {
        if (!initialized_) {
            mean_ = value;
            var_ = 0.0;
            initialized_ = true;
        } else {
            double delta = value - mean_;
            mean_ = alpha_ * value + (1.0 - alpha_) * mean_;
            var_ = (1.0 - alpha_) * (var_ + alpha_ * delta * delta);
        }
        return var_;
    }

    double variance() const { return var_; }
    double stddev() const { return std::sqrt(var_); }
    double mean() const { return mean_; }

    void reset() {
        initialized_ = false;
        mean_ = 0.0;
        var_ = 0.0;
    }

private:
    double alpha_;
    bool initialized_;
    double mean_;
    double var_;
};

// ============================================================================
// Additional Mathematical Utilities
// ============================================================================

/**
 * @brief Linear regression result
 */
struct LinearRegressionResult {
    double slope;
    double intercept;
    double r_squared;
    double std_error;
};

/**
 * @brief Calculate simple linear regression
 */
template<typename Container>
LinearRegressionResult linear_regression(const Container& y) {
    size_t n = y.size();
    if (n < 2) {
        return {0.0, 0.0, 0.0, 0.0};
    }

    // Create x values (0, 1, 2, ...)
    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
    size_t i = 0;
    for (const auto& yi : y) {
        double x = static_cast<double>(i);
        sum_x += x;
        sum_y += yi;
        sum_xy += x * yi;
        sum_xx += x * x;
        ++i;
    }

    double n_d = static_cast<double>(n);
    double denom = n_d * sum_xx - sum_x * sum_x;

    if (std::abs(denom) < 1e-10) {
        return {0.0, sum_y / n_d, 0.0, 0.0};
    }

    double slope = (n_d * sum_xy - sum_x * sum_y) / denom;
    double intercept = (sum_y - slope * sum_x) / n_d;

    // Calculate R-squared
    double ss_tot = 0.0, ss_res = 0.0;
    double mean_y = sum_y / n_d;
    i = 0;
    for (const auto& yi : y) {
        double y_pred = slope * i + intercept;
        ss_res += (yi - y_pred) * (yi - y_pred);
        ss_tot += (yi - mean_y) * (yi - mean_y);
        ++i;
    }

    double r_squared = (ss_tot > 0) ? 1.0 - ss_res / ss_tot : 0.0;
    double std_error = (n > 2) ? std::sqrt(ss_res / (n - 2)) : 0.0;

    return {slope, intercept, r_squared, std_error};
}

/**
 * @brief Calculate half-life of mean reversion
 */
inline double half_life(double lambda) {
    if (lambda <= 0.0 || lambda >= 1.0) return std::numeric_limits<double>::infinity();
    return -std::log(2.0) / std::log(1.0 - lambda);
}

/**
 * @brief Estimate half-life from time series using OLS
 */
template<typename Container>
double estimate_half_life(const Container& prices) {
    if (prices.size() < 3) return std::numeric_limits<double>::infinity();

    // Calculate lagged differences
    std::vector<double> y;
    std::vector<double> x;

    auto it = prices.begin();
    double prev = *it++;

    for (; it != prices.end(); ++it) {
        double curr = *it;
        double lag = prev;
        double diff = curr - prev;

        x.push_back(lag);
        y.push_back(diff);

        prev = curr;
    }

    // Linear regression: diff = alpha + beta * lag
    auto result = linear_regression(y);

    // In this simple case, we need proper OLS with x as independent variable
    // Simplified: beta is approximately -lambda for mean-reverting series
    double mean_x = mean(x);
    double mean_y = mean(y);

    double num = 0.0, denom = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        num += (x[i] - mean_x) * (y[i] - mean_y);
        denom += (x[i] - mean_x) * (x[i] - mean_x);
    }

    if (denom == 0.0) return std::numeric_limits<double>::infinity();

    double beta = num / denom;

    if (beta >= 0.0) return std::numeric_limits<double>::infinity();

    return -std::log(2.0) / std::log(1.0 + beta);
}

/**
 * @brief Clip value to range
 */
template<typename T>
constexpr T clip(T value, T min_val, T max_val) {
    return std::max(min_val, std::min(max_val, value));
}

/**
 * @brief Safe division (returns default on division by zero)
 */
inline double safe_div(double num, double denom, double default_val = 0.0) {
    return (denom != 0.0) ? num / denom : default_val;
}

/**
 * @brief Check if value is approximately zero
 */
inline bool is_zero(double value, double epsilon = 1e-10) {
    return std::abs(value) < epsilon;
}

/**
 * @brief Check if two values are approximately equal
 */
inline bool approx_equal(double a, double b, double epsilon = 1e-10) {
    return std::abs(a - b) < epsilon;
}

/**
 * @brief Sign function
 */
template<typename T>
constexpr int sign(T value) {
    return (T(0) < value) - (value < T(0));
}

/**
 * @brief Calculate returns from prices
 */
template<typename Container>
std::vector<double> calculate_returns(const Container& prices, bool log_returns = false) {
    std::vector<double> returns;
    if (prices.size() < 2) return returns;

    returns.reserve(prices.size() - 1);

    auto it = prices.begin();
    double prev = *it++;

    for (; it != prices.end(); ++it) {
        double curr = *it;
        if (prev > 0) {
            if (log_returns) {
                returns.push_back(std::log(curr / prev));
            } else {
                returns.push_back((curr - prev) / prev);
            }
        } else {
            returns.push_back(0.0);
        }
        prev = curr;
    }

    return returns;
}

/**
 * @brief Calculate Sharpe ratio
 */
template<typename Container>
double sharpe_ratio(const Container& returns, double risk_free_rate = 0.0,
                    double periods_per_year = 252.0) {
    if (returns.size() < 2) return 0.0;

    double m = mean(returns) - risk_free_rate / periods_per_year;
    double s = stddev(returns);

    if (s == 0.0) return 0.0;

    return m / s * std::sqrt(periods_per_year);
}

/**
 * @brief Calculate maximum drawdown
 */
template<typename Container>
double max_drawdown(const Container& equity_curve) {
    if (equity_curve.empty()) return 0.0;

    double peak = equity_curve.front();
    double max_dd = 0.0;

    for (const auto& value : equity_curve) {
        if (value > peak) {
            peak = value;
        }
        double dd = (peak - value) / peak;
        if (dd > max_dd) {
            max_dd = dd;
        }
    }

    return max_dd;
}

} // namespace utils
} // namespace hft
