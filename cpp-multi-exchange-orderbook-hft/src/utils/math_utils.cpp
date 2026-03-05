/**
 * @file math_utils.cpp
 * @brief Implementation of mathematical utilities
 *
 * Most functions are implemented as templates in the header.
 * This file contains any non-template implementations and explicit instantiations.
 */

#include "utils/math_utils.hpp"

namespace hft {
namespace utils {

// ============================================================================
// Explicit Template Instantiations for common types
// ============================================================================

// FixedPoint instantiations
template class FixedPoint<8>;  // Price, Quantity
template class FixedPoint<6>;  // Percent
template class FixedPoint<2>;  // Low precision
template class FixedPoint<4>;  // Medium precision

// ============================================================================
// Additional utility functions that benefit from being in cpp
// ============================================================================

namespace detail {

/**
 * @brief Numerically stable variance calculation using two-pass algorithm
 * Used for very large datasets where Welford's online algorithm might accumulate error
 */
double stable_variance(const double* data, size_t n, bool sample) {
    if (n < 2) return 0.0;

    // First pass: compute mean
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += data[i];
    }
    double mean = sum / n;

    // Second pass: compute variance with shifted data
    double sum_sq = 0.0;
    double sum_diff = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double diff = data[i] - mean;
        sum_sq += diff * diff;
        sum_diff += diff;
    }

    // Correction for numerical stability
    double correction = (sum_diff * sum_diff) / n;
    double variance = (sum_sq - correction) / (sample ? n - 1 : n);

    return variance;
}

/**
 * @brief Fast approximation of exp(-x) for x >= 0
 * Using Pade approximation for better accuracy
 */
double fast_exp_neg(double x) {
    if (x <= 0) return 1.0;
    if (x > 10) return 0.0;  // Effectively zero

    // Reduce to exp(-y) where 0 <= y < ln(2)
    const double ln2 = 0.693147180559945;
    int k = static_cast<int>(x / ln2);
    double y = x - k * ln2;

    // Pade approximation for exp(-y)
    double y2 = y * y;
    double num = 1.0 - y * (0.5 - y * (0.0833333333 - y * 0.0138888889));
    double den = 1.0 + y * (0.5 + y * (0.0833333333 + y * 0.0138888889));
    double exp_y = num / den;

    // Combine: exp(-x) = exp(-y) * 2^(-k)
    return std::ldexp(exp_y, -k);
}

/**
 * @brief Fast approximation of log(1 + x) for small x
 */
double fast_log1p(double x) {
    if (std::abs(x) < 1e-4) {
        // Taylor series for small x
        return x * (1.0 - x * (0.5 - x * (1.0/3.0 - x * 0.25)));
    }
    return std::log1p(x);
}

/**
 * @brief Fast inverse square root (Quake-style, modernized)
 */
double fast_inv_sqrt(double x) {
    if (x <= 0) return 0.0;

    // Initial approximation using bit manipulation
    int64_t i;
    double y;
    std::memcpy(&i, &x, sizeof(i));
    i = 0x5fe6eb50c7b537a9LL - (i >> 1);
    std::memcpy(&y, &i, sizeof(y));

    // Newton-Raphson refinement (2 iterations for double precision)
    y = y * (1.5 - 0.5 * x * y * y);
    y = y * (1.5 - 0.5 * x * y * y);

    return y;
}

} // namespace detail

// ============================================================================
// Pre-computed lookup tables for common operations
// ============================================================================

namespace {

// Lookup table for sqrt(x) where x = i/1000 for i in [0, 1000]
// Useful for normalizing values in [0, 1]
class SqrtLookupTable {
public:
    static constexpr size_t SIZE = 1001;

    SqrtLookupTable() {
        for (size_t i = 0; i < SIZE; ++i) {
            table_[i] = std::sqrt(static_cast<double>(i) / 1000.0);
        }
    }

    double lookup(double x) const {
        if (x < 0) return 0.0;
        if (x >= 1.0) return std::sqrt(x);

        double idx = x * 1000.0;
        size_t i = static_cast<size_t>(idx);
        double frac = idx - i;

        if (i >= SIZE - 1) return table_[SIZE - 1];

        // Linear interpolation
        return table_[i] * (1.0 - frac) + table_[i + 1] * frac;
    }

private:
    std::array<double, SIZE> table_;
};

// Global lookup table instance
const SqrtLookupTable sqrt_table;

} // anonymous namespace

/**
 * @brief Fast sqrt for values in [0, 1] using lookup table
 */
double fast_sqrt_unit(double x) {
    return sqrt_table.lookup(x);
}

// ============================================================================
// Robust Statistical Functions
// ============================================================================

/**
 * @brief Median Absolute Deviation (MAD)
 * More robust than standard deviation for detecting outliers
 */
double median_absolute_deviation(const std::vector<double>& data) {
    if (data.empty()) return 0.0;

    double med = median(data);

    std::vector<double> abs_devs;
    abs_devs.reserve(data.size());

    for (double x : data) {
        abs_devs.push_back(std::abs(x - med));
    }

    return median(abs_devs);
}

/**
 * @brief Winsorize data (clip outliers to percentile bounds)
 */
std::vector<double> winsorize(const std::vector<double>& data,
                               double lower_percentile,
                               double upper_percentile) {
    if (data.empty()) return {};

    double lower = percentile(data, lower_percentile);
    double upper = percentile(data, upper_percentile);

    std::vector<double> result;
    result.reserve(data.size());

    for (double x : data) {
        result.push_back(clip(x, lower, upper));
    }

    return result;
}

/**
 * @brief Calculate weighted average
 */
double weighted_average(const std::vector<double>& values,
                        const std::vector<double>& weights) {
    if (values.empty() || values.size() != weights.size()) return 0.0;

    double sum_weighted = 0.0;
    double sum_weights = 0.0;

    for (size_t i = 0; i < values.size(); ++i) {
        sum_weighted += values[i] * weights[i];
        sum_weights += weights[i];
    }

    return (sum_weights > 0) ? sum_weighted / sum_weights : 0.0;
}

/**
 * @brief Calculate exponentially weighted covariance matrix (for 2 series)
 */
std::array<double, 4> ewm_cov_matrix(const std::vector<double>& x,
                                      const std::vector<double>& y,
                                      double span) {
    // Returns [var_x, cov_xy, cov_xy, var_y] (row-major 2x2)
    if (x.size() != y.size() || x.empty()) {
        return {0.0, 0.0, 0.0, 0.0};
    }

    double alpha = 2.0 / (span + 1.0);

    double mean_x = x[0];
    double mean_y = y[0];
    double var_x = 0.0;
    double var_y = 0.0;
    double cov_xy = 0.0;

    for (size_t i = 1; i < x.size(); ++i) {
        double dx = x[i] - mean_x;
        double dy = y[i] - mean_y;

        mean_x = alpha * x[i] + (1 - alpha) * mean_x;
        mean_y = alpha * y[i] + (1 - alpha) * mean_y;

        var_x = (1 - alpha) * (var_x + alpha * dx * dx);
        var_y = (1 - alpha) * (var_y + alpha * dy * dy);
        cov_xy = (1 - alpha) * (cov_xy + alpha * dx * dy);
    }

    return {var_x, cov_xy, cov_xy, var_y};
}

} // namespace utils
} // namespace hft
