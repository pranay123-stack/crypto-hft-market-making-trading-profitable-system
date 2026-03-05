#pragma once

/**
 * @file performance_analyzer.hpp
 * @brief Comprehensive performance analysis for backtesting
 *
 * Provides detailed performance metrics, equity curve analysis,
 * trade-by-trade analysis, and report generation.
 */

#include "core/types.hpp"
#include "backtesting/simulated_exchange.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <optional>
#include <cmath>
#include <numeric>

namespace hft::backtesting {

// ============================================================================
// Trade Record
// ============================================================================

/**
 * @brief Complete trade record for analysis
 */
struct TradeRecord {
    uint64_t trade_id{0};
    core::Symbol symbol;
    core::Exchange exchange;

    core::Side entry_side;
    core::Timestamp entry_time;
    core::Price entry_price;
    core::Quantity quantity;

    core::Timestamp exit_time;
    core::Price exit_price;

    double realized_pnl{0.0};       // Profit/loss in quote currency
    double realized_pnl_pct{0.0};   // Percentage return
    double fees{0.0};
    double slippage{0.0};

    uint64_t duration_us{0};        // Trade duration in microseconds
    uint32_t fills{0};              // Number of fills

    [[nodiscard]] bool is_winner() const noexcept {
        return realized_pnl > 0.0;
    }

    [[nodiscard]] double gross_pnl() const noexcept {
        return realized_pnl + fees;
    }

    [[nodiscard]] double notional() const noexcept {
        return entry_price.to_double() * quantity.to_double();
    }

    [[nodiscard]] double duration_seconds() const noexcept {
        return static_cast<double>(duration_us) / 1'000'000.0;
    }

    [[nodiscard]] double duration_minutes() const noexcept {
        return duration_seconds() / 60.0;
    }

    [[nodiscard]] double duration_hours() const noexcept {
        return duration_minutes() / 60.0;
    }
};

// ============================================================================
// Equity Point
// ============================================================================

/**
 * @brief Single point on the equity curve
 */
struct EquityPoint {
    core::Timestamp timestamp;
    double equity{0.0};             // Total account equity
    double cash{0.0};               // Cash balance
    double positions_value{0.0};    // Value of open positions
    double unrealized_pnl{0.0};
    double realized_pnl{0.0};
    double drawdown{0.0};           // Current drawdown from peak
    double drawdown_pct{0.0};       // Drawdown as percentage
    uint32_t open_positions{0};
    uint64_t trade_count{0};

    [[nodiscard]] double total_pnl() const noexcept {
        return realized_pnl + unrealized_pnl;
    }
};

// ============================================================================
// Drawdown Analysis
// ============================================================================

/**
 * @brief Drawdown period information
 */
struct DrawdownPeriod {
    core::Timestamp start_time;
    core::Timestamp trough_time;
    core::Timestamp end_time;       // When recovered (0 if ongoing)

    double peak_equity{0.0};
    double trough_equity{0.0};
    double drawdown{0.0};           // Absolute drawdown
    double drawdown_pct{0.0};       // Percentage drawdown

    uint64_t duration_to_trough_us{0};
    uint64_t duration_to_recovery_us{0};

    [[nodiscard]] bool is_recovered() const noexcept {
        return end_time.nanos > 0;
    }

    [[nodiscard]] double recovery_factor() const noexcept {
        if (drawdown == 0.0) return 0.0;
        return (trough_equity - peak_equity) / drawdown;
    }
};

// ============================================================================
// Performance Metrics
// ============================================================================

/**
 * @brief Comprehensive performance metrics
 */
struct PerformanceMetrics {
    // Basic returns
    double total_return{0.0};           // Total return in quote currency
    double total_return_pct{0.0};       // Percentage return
    double annualized_return{0.0};      // Annualized return
    double cagr{0.0};                   // Compound annual growth rate

    // Risk metrics
    double volatility{0.0};             // Annualized volatility
    double downside_volatility{0.0};    // Downside deviation
    double max_drawdown{0.0};           // Maximum drawdown (absolute)
    double max_drawdown_pct{0.0};       // Maximum drawdown (percentage)
    double average_drawdown{0.0};       // Average drawdown
    double ulcer_index{0.0};            // Ulcer Index

    // Risk-adjusted returns
    double sharpe_ratio{0.0};           // Sharpe ratio (vs risk-free rate)
    double sortino_ratio{0.0};          // Sortino ratio
    double calmar_ratio{0.0};           // Calmar ratio (return/max_dd)
    double information_ratio{0.0};      // Information ratio vs benchmark
    double omega_ratio{0.0};            // Omega ratio

    // Trade statistics
    uint64_t total_trades{0};
    uint64_t winning_trades{0};
    uint64_t losing_trades{0};
    uint64_t break_even_trades{0};
    double win_rate{0.0};

    double profit_factor{0.0};          // Gross profit / gross loss
    double expectancy{0.0};             // Expected value per trade
    double expectancy_pct{0.0};         // Expected value as percentage

    double average_win{0.0};            // Average winning trade
    double average_loss{0.0};           // Average losing trade
    double largest_win{0.0};
    double largest_loss{0.0};
    double average_trade{0.0};          // Average P/L per trade

    double payoff_ratio{0.0};           // Average win / average loss

    // Duration statistics
    double average_trade_duration_us{0.0};
    double average_winner_duration_us{0.0};
    double average_loser_duration_us{0.0};
    double max_trade_duration_us{0.0};
    double min_trade_duration_us{0.0};

    // Volume and fees
    double total_volume{0.0};           // Total traded volume
    double total_fees{0.0};
    double fees_as_pct_of_pnl{0.0};

    // Time-based metrics
    double trades_per_day{0.0};
    double volume_per_day{0.0};
    uint64_t profitable_days{0};
    uint64_t losing_days{0};
    double best_day{0.0};
    double worst_day{0.0};
    double average_daily_return{0.0};

    // Streak analysis
    uint32_t max_consecutive_wins{0};
    uint32_t max_consecutive_losses{0};
    uint32_t current_win_streak{0};
    uint32_t current_loss_streak{0};

    // Recovery metrics
    double max_recovery_time_us{0.0};   // Longest time to recover from drawdown
    double average_recovery_time_us{0.0};

    // Position metrics
    double average_position_size{0.0};
    double max_position_size{0.0};
    double average_exposure{0.0};       // Average % of capital deployed
    double max_exposure{0.0};
};

/**
 * @brief Monthly/periodic return data
 */
struct PeriodReturn {
    core::Timestamp start;
    core::Timestamp end;
    double return_pct{0.0};
    double return_abs{0.0};
    uint64_t trades{0};
    double max_drawdown_pct{0.0};
};

// ============================================================================
// Benchmark Comparison
// ============================================================================

/**
 * @brief Benchmark comparison metrics
 */
struct BenchmarkComparison {
    std::string benchmark_name;
    double benchmark_return{0.0};
    double alpha{0.0};                  // Excess return over benchmark
    double beta{0.0};                   // Sensitivity to benchmark
    double correlation{0.0};            // Correlation with benchmark
    double tracking_error{0.0};         // Standard deviation of alpha
    double information_ratio{0.0};      // Alpha / tracking error
    double treynor_ratio{0.0};          // Excess return / beta
};

// ============================================================================
// Report Configuration
// ============================================================================

/**
 * @brief Report output configuration
 */
struct ReportConfig {
    enum class Format {
        Text,
        JSON,
        CSV,
        HTML
    };

    Format format{Format::Text};
    std::string output_path;

    bool include_equity_curve{true};
    bool include_trade_list{true};
    bool include_drawdown_analysis{true};
    bool include_monthly_returns{true};
    bool include_daily_returns{false};
    bool include_benchmark_comparison{true};

    // Equity curve sampling
    uint64_t equity_sample_interval_us{60'000'000};  // 1 minute default

    // Risk-free rate for Sharpe calculation
    double risk_free_rate{0.02};  // 2% annual

    // Benchmark data path (optional)
    std::string benchmark_path;
};

// ============================================================================
// Performance Analyzer
// ============================================================================

/**
 * @brief Main performance analysis class
 *
 * Collects data during backtest and computes comprehensive metrics.
 */
class PerformanceAnalyzer {
public:
    explicit PerformanceAnalyzer(double initial_capital = 100000.0);
    ~PerformanceAnalyzer() = default;

    // Reset analyzer
    void reset(double initial_capital);

    // Data recording (called during backtest)
    void recordFill(const Fill& fill);
    void recordEquityPoint(const EquityPoint& point);
    void recordTradeClose(const TradeRecord& trade);
    void recordDailyClose(core::Timestamp timestamp, double equity);

    // Simplified recording
    void updateEquity(core::Timestamp timestamp, double equity,
                      double unrealized_pnl = 0.0, uint32_t open_positions = 0);

    // Compute all metrics
    void compute();

    // Access metrics
    [[nodiscard]] const PerformanceMetrics& metrics() const { return metrics_; }
    [[nodiscard]] const std::vector<TradeRecord>& trades() const { return trades_; }
    [[nodiscard]] const std::vector<EquityPoint>& equityCurve() const { return equity_curve_; }
    [[nodiscard]] const std::vector<DrawdownPeriod>& drawdowns() const { return drawdowns_; }
    [[nodiscard]] const std::vector<PeriodReturn>& monthlyReturns() const { return monthly_returns_; }
    [[nodiscard]] const std::vector<PeriodReturn>& dailyReturns() const { return daily_returns_; }

    // Benchmark comparison
    void setBenchmark(const std::string& name,
                      const std::vector<std::pair<core::Timestamp, double>>& returns);
    [[nodiscard]] const BenchmarkComparison& benchmarkComparison() const {
        return benchmark_comparison_;
    }

    // Report generation
    [[nodiscard]] std::string generateReport(const ReportConfig& config) const;
    void saveReport(const ReportConfig& config) const;

    // JSON export
    [[nodiscard]] std::string toJSON() const;

    // Configuration
    void setRiskFreeRate(double rate) { risk_free_rate_ = rate; }
    [[nodiscard]] double riskFreeRate() const { return risk_free_rate_; }

    // Summary statistics
    [[nodiscard]] double initialCapital() const { return initial_capital_; }
    [[nodiscard]] double finalEquity() const;
    [[nodiscard]] core::Timestamp startTime() const;
    [[nodiscard]] core::Timestamp endTime() const;
    [[nodiscard]] uint64_t durationDays() const;

private:
    // Metric computation helpers
    void computeReturnMetrics();
    void computeRiskMetrics();
    void computeRiskAdjustedMetrics();
    void computeTradeStatistics();
    void computeDrawdowns();
    void computeStreaks();
    void computeTimeBasedMetrics();
    void computeBenchmarkMetrics();

    // Statistical helpers
    [[nodiscard]] double computeMean(const std::vector<double>& values) const;
    [[nodiscard]] double computeStdDev(const std::vector<double>& values, double mean) const;
    [[nodiscard]] double computeDownsideDeviation(const std::vector<double>& values,
                                                   double threshold) const;
    [[nodiscard]] double computeCorrelation(const std::vector<double>& x,
                                            const std::vector<double>& y) const;
    [[nodiscard]] double computePercentile(std::vector<double> values, double percentile) const;

    // Report generation helpers
    [[nodiscard]] std::string generateTextReport(const ReportConfig& config) const;
    [[nodiscard]] std::string generateJSONReport(const ReportConfig& config) const;
    [[nodiscard]] std::string generateCSVReport(const ReportConfig& config) const;
    [[nodiscard]] std::string generateHTMLReport(const ReportConfig& config) const;

    // Data storage
    double initial_capital_{100000.0};
    double risk_free_rate_{0.02};

    std::vector<TradeRecord> trades_;
    std::vector<EquityPoint> equity_curve_;
    std::vector<DrawdownPeriod> drawdowns_;
    std::vector<PeriodReturn> monthly_returns_;
    std::vector<PeriodReturn> daily_returns_;
    std::vector<Fill> fills_;

    // Daily P/L tracking
    std::map<uint64_t, double> daily_pnl_;  // Day number -> P/L

    // Benchmark data
    std::vector<std::pair<core::Timestamp, double>> benchmark_returns_;
    BenchmarkComparison benchmark_comparison_;

    // Computed metrics
    PerformanceMetrics metrics_;
    bool computed_{false};
};

// ============================================================================
// Trade Tracker (Helper for tracking position -> trade conversion)
// ============================================================================

/**
 * @brief Tracks open positions and converts fills to complete trades
 */
class TradeTracker {
public:
    TradeTracker() = default;

    // Process a fill
    void processFill(const Fill& fill, double current_price);

    // Get completed trades
    [[nodiscard]] std::vector<TradeRecord> getCompletedTrades();

    // Get open position info
    [[nodiscard]] bool hasOpenPosition(const core::Symbol& symbol) const;
    [[nodiscard]] double getOpenQuantity(const core::Symbol& symbol) const;
    [[nodiscard]] double getAverageEntryPrice(const core::Symbol& symbol) const;

    // Clear all tracking
    void clear();

private:
    struct OpenPosition {
        core::Symbol symbol;
        core::Exchange exchange;
        core::Side side;
        core::Timestamp entry_time;
        double quantity{0.0};
        double average_price{0.0};
        double total_cost{0.0};
        double total_fees{0.0};
        uint32_t fill_count{0};
    };

    std::unordered_map<std::string, OpenPosition> open_positions_;
    std::vector<TradeRecord> completed_trades_;
    uint64_t next_trade_id_{1};
};

// ============================================================================
// Equity Curve Builder
// ============================================================================

/**
 * @brief Builds equity curve from fills and market data
 */
class EquityCurveBuilder {
public:
    explicit EquityCurveBuilder(double initial_capital);

    // Update with new data
    void update(core::Timestamp timestamp, double portfolio_value,
                double realized_pnl, double unrealized_pnl,
                uint32_t open_positions, uint64_t trade_count);

    // Build final curve
    [[nodiscard]] std::vector<EquityPoint> build() const;

    // Sample at regular intervals
    [[nodiscard]] std::vector<EquityPoint> buildSampled(uint64_t interval_us) const;

    // Get current metrics
    [[nodiscard]] double currentEquity() const;
    [[nodiscard]] double currentDrawdown() const;
    [[nodiscard]] double currentDrawdownPct() const;
    [[nodiscard]] double peakEquity() const;

private:
    double initial_capital_;
    double peak_equity_;
    std::vector<EquityPoint> points_;
};

// ============================================================================
// Risk Metrics Calculator
// ============================================================================

/**
 * @brief Utility class for computing risk metrics
 */
class RiskMetrics {
public:
    // Sharpe ratio
    [[nodiscard]] static double sharpeRatio(
        const std::vector<double>& returns,
        double risk_free_rate = 0.02,
        double periods_per_year = 252.0);

    // Sortino ratio
    [[nodiscard]] static double sortinoRatio(
        const std::vector<double>& returns,
        double target_return = 0.0,
        double periods_per_year = 252.0);

    // Calmar ratio
    [[nodiscard]] static double calmarRatio(
        double annualized_return,
        double max_drawdown);

    // Maximum drawdown
    [[nodiscard]] static double maxDrawdown(const std::vector<double>& equity_curve);
    [[nodiscard]] static double maxDrawdownPct(const std::vector<double>& equity_curve);

    // VaR (Value at Risk)
    [[nodiscard]] static double valueAtRisk(
        const std::vector<double>& returns,
        double confidence = 0.95);

    // CVaR (Conditional VaR / Expected Shortfall)
    [[nodiscard]] static double conditionalVaR(
        const std::vector<double>& returns,
        double confidence = 0.95);

    // Omega ratio
    [[nodiscard]] static double omegaRatio(
        const std::vector<double>& returns,
        double threshold = 0.0);

    // Information ratio
    [[nodiscard]] static double informationRatio(
        const std::vector<double>& strategy_returns,
        const std::vector<double>& benchmark_returns);

    // Beta
    [[nodiscard]] static double beta(
        const std::vector<double>& strategy_returns,
        const std::vector<double>& benchmark_returns);

    // Alpha
    [[nodiscard]] static double alpha(
        const std::vector<double>& strategy_returns,
        const std::vector<double>& benchmark_returns,
        double risk_free_rate = 0.02);

    // Annualized volatility
    [[nodiscard]] static double annualizedVolatility(
        const std::vector<double>& returns,
        double periods_per_year = 252.0);

    // Ulcer Index
    [[nodiscard]] static double ulcerIndex(const std::vector<double>& prices);
};

}  // namespace hft::backtesting
