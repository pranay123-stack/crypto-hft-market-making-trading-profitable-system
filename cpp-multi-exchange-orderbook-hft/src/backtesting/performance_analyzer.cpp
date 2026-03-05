/**
 * @file performance_analyzer.cpp
 * @brief Performance analysis implementation
 */

#include "backtesting/performance_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <fstream>

namespace hft::backtesting {

// ============================================================================
// PerformanceAnalyzer Implementation
// ============================================================================

PerformanceAnalyzer::PerformanceAnalyzer(double initial_capital)
    : initial_capital_(initial_capital) {
}

void PerformanceAnalyzer::reset(double initial_capital) {
    initial_capital_ = initial_capital;
    trades_.clear();
    equity_curve_.clear();
    drawdowns_.clear();
    monthly_returns_.clear();
    daily_returns_.clear();
    fills_.clear();
    daily_pnl_.clear();
    benchmark_returns_.clear();
    metrics_ = PerformanceMetrics{};
    benchmark_comparison_ = BenchmarkComparison{};
    computed_ = false;
}

void PerformanceAnalyzer::recordFill(const Fill& fill) {
    fills_.push_back(fill);
}

void PerformanceAnalyzer::recordEquityPoint(const EquityPoint& point) {
    equity_curve_.push_back(point);
}

void PerformanceAnalyzer::recordTradeClose(const TradeRecord& trade) {
    trades_.push_back(trade);
}

void PerformanceAnalyzer::recordDailyClose(core::Timestamp timestamp, double equity) {
    uint64_t day = timestamp.to_seconds() / 86400;
    daily_pnl_[day] = equity;
}

void PerformanceAnalyzer::updateEquity(core::Timestamp timestamp, double equity,
                                        double unrealized_pnl, uint32_t open_positions) {
    EquityPoint point;
    point.timestamp = timestamp;
    point.equity = equity;
    point.unrealized_pnl = unrealized_pnl;
    point.open_positions = open_positions;
    point.trade_count = trades_.size();

    // Calculate realized P/L
    if (!equity_curve_.empty()) {
        point.realized_pnl = equity - initial_capital_ - unrealized_pnl;
    }

    // Calculate drawdown
    static double peak = initial_capital_;
    if (equity > peak) {
        peak = equity;
    }
    point.drawdown = peak - equity;
    point.drawdown_pct = (peak > 0) ? (point.drawdown / peak) * 100.0 : 0.0;

    equity_curve_.push_back(point);
}

void PerformanceAnalyzer::compute() {
    if (computed_) return;

    computeReturnMetrics();
    computeRiskMetrics();
    computeRiskAdjustedMetrics();
    computeTradeStatistics();
    computeDrawdowns();
    computeStreaks();
    computeTimeBasedMetrics();

    if (!benchmark_returns_.empty()) {
        computeBenchmarkMetrics();
    }

    computed_ = true;
}

void PerformanceAnalyzer::computeReturnMetrics() {
    if (equity_curve_.empty()) return;

    double final_equity = equity_curve_.back().equity;
    metrics_.total_return = final_equity - initial_capital_;
    metrics_.total_return_pct = (metrics_.total_return / initial_capital_) * 100.0;

    // Calculate trading period in years
    uint64_t duration_us = equity_curve_.back().timestamp.nanos / 1000 -
                           equity_curve_.front().timestamp.nanos / 1000;
    double years = static_cast<double>(duration_us) / (365.25 * 24 * 60 * 60 * 1e6);

    if (years > 0) {
        // CAGR = (final/initial)^(1/years) - 1
        double ratio = final_equity / initial_capital_;
        if (ratio > 0) {
            metrics_.cagr = (std::pow(ratio, 1.0 / years) - 1.0) * 100.0;
        }
        metrics_.annualized_return = metrics_.cagr;
    }
}

void PerformanceAnalyzer::computeRiskMetrics() {
    if (equity_curve_.size() < 2) return;

    // Calculate daily returns
    std::vector<double> returns;
    for (size_t i = 1; i < equity_curve_.size(); ++i) {
        double prev_equity = equity_curve_[i - 1].equity;
        double curr_equity = equity_curve_[i].equity;
        if (prev_equity > 0) {
            returns.push_back((curr_equity - prev_equity) / prev_equity);
        }
    }

    if (returns.empty()) return;

    // Volatility (annualized)
    double mean_return = computeMean(returns);
    double std_dev = computeStdDev(returns, mean_return);

    // Estimate periods per year (assuming data is sampled consistently)
    uint64_t total_us = equity_curve_.back().timestamp.nanos / 1000 -
                        equity_curve_.front().timestamp.nanos / 1000;
    double periods = static_cast<double>(returns.size());
    double periods_per_year = periods / (static_cast<double>(total_us) /
                                         (365.25 * 24 * 60 * 60 * 1e6));

    metrics_.volatility = std_dev * std::sqrt(periods_per_year) * 100.0;

    // Downside volatility
    double downside_dev = computeDownsideDeviation(returns, 0.0);
    metrics_.downside_volatility = downside_dev * std::sqrt(periods_per_year) * 100.0;

    // Maximum drawdown
    std::vector<double> equity_values;
    for (const auto& point : equity_curve_) {
        equity_values.push_back(point.equity);
    }

    metrics_.max_drawdown = RiskMetrics::maxDrawdown(equity_values);
    metrics_.max_drawdown_pct = RiskMetrics::maxDrawdownPct(equity_values);

    // Average drawdown
    double sum_dd = 0.0;
    for (const auto& point : equity_curve_) {
        sum_dd += point.drawdown_pct;
    }
    metrics_.average_drawdown = sum_dd / equity_curve_.size();

    // Ulcer Index
    std::vector<double> prices;
    for (const auto& point : equity_curve_) {
        prices.push_back(point.equity);
    }
    metrics_.ulcer_index = RiskMetrics::ulcerIndex(prices);
}

void PerformanceAnalyzer::computeRiskAdjustedMetrics() {
    if (equity_curve_.size() < 2) return;

    // Calculate returns
    std::vector<double> returns;
    for (size_t i = 1; i < equity_curve_.size(); ++i) {
        double prev_equity = equity_curve_[i - 1].equity;
        double curr_equity = equity_curve_[i].equity;
        if (prev_equity > 0) {
            returns.push_back((curr_equity - prev_equity) / prev_equity);
        }
    }

    if (returns.empty()) return;

    // Sharpe ratio
    metrics_.sharpe_ratio = RiskMetrics::sharpeRatio(returns, risk_free_rate_);

    // Sortino ratio
    metrics_.sortino_ratio = RiskMetrics::sortinoRatio(returns, 0.0);

    // Calmar ratio
    if (metrics_.max_drawdown_pct > 0) {
        metrics_.calmar_ratio = metrics_.annualized_return / metrics_.max_drawdown_pct;
    }

    // Omega ratio
    metrics_.omega_ratio = RiskMetrics::omegaRatio(returns);
}

void PerformanceAnalyzer::computeTradeStatistics() {
    if (trades_.empty()) {
        return;
    }

    metrics_.total_trades = trades_.size();

    double total_profit = 0.0;
    double total_loss = 0.0;
    double sum_duration = 0.0;
    double sum_winner_duration = 0.0;
    double sum_loser_duration = 0.0;

    metrics_.largest_win = 0.0;
    metrics_.largest_loss = 0.0;
    metrics_.max_trade_duration_us = 0.0;
    metrics_.min_trade_duration_us = std::numeric_limits<double>::max();

    for (const auto& trade : trades_) {
        if (trade.realized_pnl > 0) {
            metrics_.winning_trades++;
            total_profit += trade.realized_pnl;
            sum_winner_duration += trade.duration_us;
            metrics_.largest_win = std::max(metrics_.largest_win, trade.realized_pnl);
        } else if (trade.realized_pnl < 0) {
            metrics_.losing_trades++;
            total_loss += std::abs(trade.realized_pnl);
            sum_loser_duration += trade.duration_us;
            metrics_.largest_loss = std::min(metrics_.largest_loss, trade.realized_pnl);
        } else {
            metrics_.break_even_trades++;
        }

        sum_duration += trade.duration_us;
        metrics_.max_trade_duration_us = std::max(
            metrics_.max_trade_duration_us, static_cast<double>(trade.duration_us));
        metrics_.min_trade_duration_us = std::min(
            metrics_.min_trade_duration_us, static_cast<double>(trade.duration_us));

        metrics_.total_fees += trade.fees;
        metrics_.total_volume += trade.notional();
    }

    // Win rate
    if (metrics_.total_trades > 0) {
        metrics_.win_rate = static_cast<double>(metrics_.winning_trades) /
                           static_cast<double>(metrics_.total_trades) * 100.0;
    }

    // Profit factor
    if (total_loss > 0) {
        metrics_.profit_factor = total_profit / total_loss;
    } else if (total_profit > 0) {
        metrics_.profit_factor = std::numeric_limits<double>::infinity();
    }

    // Averages
    if (metrics_.winning_trades > 0) {
        metrics_.average_win = total_profit / metrics_.winning_trades;
        metrics_.average_winner_duration_us = sum_winner_duration / metrics_.winning_trades;
    }

    if (metrics_.losing_trades > 0) {
        metrics_.average_loss = total_loss / metrics_.losing_trades;
        metrics_.average_loser_duration_us = sum_loser_duration / metrics_.losing_trades;
    }

    if (metrics_.total_trades > 0) {
        metrics_.average_trade = (total_profit - total_loss) / metrics_.total_trades;
        metrics_.average_trade_duration_us = sum_duration / metrics_.total_trades;
    }

    // Payoff ratio
    if (metrics_.average_loss > 0) {
        metrics_.payoff_ratio = metrics_.average_win / metrics_.average_loss;
    }

    // Expectancy
    double win_prob = metrics_.win_rate / 100.0;
    double loss_prob = 1.0 - win_prob;
    metrics_.expectancy = (win_prob * metrics_.average_win) -
                         (loss_prob * metrics_.average_loss);

    if (metrics_.average_trade != 0) {
        double avg_trade_size = metrics_.total_volume / metrics_.total_trades;
        if (avg_trade_size > 0) {
            metrics_.expectancy_pct = (metrics_.expectancy / avg_trade_size) * 100.0;
        }
    }

    // Fees as percentage of P/L
    double total_pnl = total_profit - total_loss;
    if (std::abs(total_pnl) > 0) {
        metrics_.fees_as_pct_of_pnl = (metrics_.total_fees / std::abs(total_pnl)) * 100.0;
    }

    // Position metrics
    double sum_position_size = 0.0;
    for (const auto& trade : trades_) {
        double size = trade.notional();
        sum_position_size += size;
        metrics_.max_position_size = std::max(metrics_.max_position_size, size);
    }
    metrics_.average_position_size = sum_position_size / trades_.size();
}

void PerformanceAnalyzer::computeDrawdowns() {
    if (equity_curve_.empty()) return;

    drawdowns_.clear();

    double peak = equity_curve_.front().equity;
    bool in_drawdown = false;
    DrawdownPeriod current_dd;

    for (size_t i = 0; i < equity_curve_.size(); ++i) {
        const auto& point = equity_curve_[i];

        if (point.equity >= peak) {
            // New peak or recovery
            if (in_drawdown) {
                // End of drawdown
                current_dd.end_time = point.timestamp;
                current_dd.duration_to_recovery_us =
                    (current_dd.end_time.nanos - current_dd.start_time.nanos) / 1000;
                drawdowns_.push_back(current_dd);
                in_drawdown = false;
            }
            peak = point.equity;
        } else {
            // In drawdown
            if (!in_drawdown) {
                // Start of new drawdown
                in_drawdown = true;
                current_dd = DrawdownPeriod{};
                current_dd.start_time = equity_curve_[i > 0 ? i - 1 : 0].timestamp;
                current_dd.peak_equity = peak;
                current_dd.trough_equity = point.equity;
                current_dd.trough_time = point.timestamp;
            }

            if (point.equity < current_dd.trough_equity) {
                current_dd.trough_equity = point.equity;
                current_dd.trough_time = point.timestamp;
            }

            current_dd.drawdown = current_dd.peak_equity - current_dd.trough_equity;
            current_dd.drawdown_pct = (current_dd.drawdown / current_dd.peak_equity) * 100.0;
            current_dd.duration_to_trough_us =
                (current_dd.trough_time.nanos - current_dd.start_time.nanos) / 1000;
        }
    }

    // Handle ongoing drawdown
    if (in_drawdown) {
        drawdowns_.push_back(current_dd);
    }

    // Calculate recovery metrics
    if (!drawdowns_.empty()) {
        double max_recovery = 0.0;
        double sum_recovery = 0.0;
        size_t recovered_count = 0;

        for (const auto& dd : drawdowns_) {
            if (dd.is_recovered()) {
                max_recovery = std::max(max_recovery,
                    static_cast<double>(dd.duration_to_recovery_us));
                sum_recovery += dd.duration_to_recovery_us;
                recovered_count++;
            }
        }

        metrics_.max_recovery_time_us = max_recovery;
        if (recovered_count > 0) {
            metrics_.average_recovery_time_us = sum_recovery / recovered_count;
        }
    }
}

void PerformanceAnalyzer::computeStreaks() {
    if (trades_.empty()) return;

    metrics_.max_consecutive_wins = 0;
    metrics_.max_consecutive_losses = 0;
    metrics_.current_win_streak = 0;
    metrics_.current_loss_streak = 0;

    uint32_t current_wins = 0;
    uint32_t current_losses = 0;

    for (const auto& trade : trades_) {
        if (trade.realized_pnl > 0) {
            current_wins++;
            current_losses = 0;
            metrics_.max_consecutive_wins = std::max(
                metrics_.max_consecutive_wins, current_wins);
        } else if (trade.realized_pnl < 0) {
            current_losses++;
            current_wins = 0;
            metrics_.max_consecutive_losses = std::max(
                metrics_.max_consecutive_losses, current_losses);
        }
    }

    // Current streaks
    if (!trades_.empty()) {
        const auto& last_trade = trades_.back();
        if (last_trade.realized_pnl > 0) {
            metrics_.current_win_streak = current_wins;
        } else if (last_trade.realized_pnl < 0) {
            metrics_.current_loss_streak = current_losses;
        }
    }
}

void PerformanceAnalyzer::computeTimeBasedMetrics() {
    if (equity_curve_.empty()) return;

    // Calculate duration in days
    uint64_t duration_us = equity_curve_.back().timestamp.nanos / 1000 -
                           equity_curve_.front().timestamp.nanos / 1000;
    double days = static_cast<double>(duration_us) / (24.0 * 60 * 60 * 1e6);

    if (days > 0) {
        metrics_.trades_per_day = static_cast<double>(trades_.size()) / days;
        metrics_.volume_per_day = metrics_.total_volume / days;
    }

    // Daily returns analysis
    if (!daily_pnl_.empty()) {
        std::vector<double> daily_returns;
        double prev_equity = initial_capital_;

        for (const auto& [day, equity] : daily_pnl_) {
            double daily_ret = (equity - prev_equity) / prev_equity;
            daily_returns.push_back(daily_ret);

            if (daily_ret > 0) {
                metrics_.profitable_days++;
            } else if (daily_ret < 0) {
                metrics_.losing_days++;
            }

            metrics_.best_day = std::max(metrics_.best_day, daily_ret * 100.0);
            metrics_.worst_day = std::min(metrics_.worst_day, daily_ret * 100.0);

            prev_equity = equity;
        }

        if (!daily_returns.empty()) {
            metrics_.average_daily_return = computeMean(daily_returns) * 100.0;
        }

        // Generate daily return periods
        prev_equity = initial_capital_;
        uint64_t trade_idx = 0;

        for (const auto& [day, equity] : daily_pnl_) {
            PeriodReturn pr;
            pr.start = core::Timestamp::from_seconds(day * 86400);
            pr.end = core::Timestamp::from_seconds((day + 1) * 86400);
            pr.return_abs = equity - prev_equity;
            pr.return_pct = (prev_equity > 0) ? (pr.return_abs / prev_equity) * 100.0 : 0.0;

            // Count trades for this day
            while (trade_idx < trades_.size() &&
                   trades_[trade_idx].exit_time <= pr.end) {
                pr.trades++;
                trade_idx++;
            }

            daily_returns_.push_back(pr);
            prev_equity = equity;
        }
    }

    // Generate monthly returns
    if (equity_curve_.size() >= 2) {
        std::map<uint64_t, std::vector<EquityPoint>> monthly_points;

        for (const auto& point : equity_curve_) {
            uint64_t seconds = point.timestamp.to_seconds();
            std::time_t time = static_cast<std::time_t>(seconds);
            std::tm* tm = std::gmtime(&time);
            uint64_t month_key = tm->tm_year * 12 + tm->tm_mon;
            monthly_points[month_key].push_back(point);
        }

        double prev_month_equity = initial_capital_;

        for (auto& [month_key, points] : monthly_points) {
            if (points.empty()) continue;

            // Sort by timestamp
            std::sort(points.begin(), points.end(),
                [](const EquityPoint& a, const EquityPoint& b) {
                    return a.timestamp < b.timestamp;
                });

            PeriodReturn mr;
            mr.start = points.front().timestamp;
            mr.end = points.back().timestamp;

            double end_equity = points.back().equity;
            mr.return_abs = end_equity - prev_month_equity;
            mr.return_pct = (prev_month_equity > 0) ?
                           (mr.return_abs / prev_month_equity) * 100.0 : 0.0;

            // Count trades in this month
            for (const auto& trade : trades_) {
                if (trade.exit_time >= mr.start && trade.exit_time <= mr.end) {
                    mr.trades++;
                }
            }

            // Max drawdown in month
            double month_peak = prev_month_equity;
            double month_max_dd = 0.0;
            for (const auto& point : points) {
                month_peak = std::max(month_peak, point.equity);
                double dd = (month_peak - point.equity) / month_peak;
                month_max_dd = std::max(month_max_dd, dd);
            }
            mr.max_drawdown_pct = month_max_dd * 100.0;

            monthly_returns_.push_back(mr);
            prev_month_equity = end_equity;
        }
    }

    // Calculate average exposure
    if (!equity_curve_.empty()) {
        double sum_exposure = 0.0;
        double max_exposure = 0.0;

        for (const auto& point : equity_curve_) {
            double exposure = point.positions_value / point.equity;
            sum_exposure += exposure;
            max_exposure = std::max(max_exposure, exposure);
        }

        metrics_.average_exposure = (sum_exposure / equity_curve_.size()) * 100.0;
        metrics_.max_exposure = max_exposure * 100.0;
    }
}

void PerformanceAnalyzer::computeBenchmarkMetrics() {
    if (benchmark_returns_.empty() || equity_curve_.size() < 2) return;

    // Calculate strategy returns aligned with benchmark
    std::vector<double> strategy_returns;
    std::vector<double> bench_returns;

    size_t bench_idx = 0;
    for (size_t i = 1; i < equity_curve_.size() && bench_idx < benchmark_returns_.size(); ++i) {
        // Find matching benchmark point
        while (bench_idx < benchmark_returns_.size() - 1 &&
               benchmark_returns_[bench_idx + 1].first <= equity_curve_[i].timestamp) {
            bench_idx++;
        }

        if (bench_idx < benchmark_returns_.size()) {
            double strat_ret = (equity_curve_[i].equity - equity_curve_[i - 1].equity) /
                              equity_curve_[i - 1].equity;
            strategy_returns.push_back(strat_ret);
            bench_returns.push_back(benchmark_returns_[bench_idx].second);
        }
    }

    if (strategy_returns.empty()) return;

    // Calculate total benchmark return
    double bench_total = 1.0;
    for (double r : bench_returns) {
        bench_total *= (1.0 + r);
    }
    benchmark_comparison_.benchmark_return = (bench_total - 1.0) * 100.0;

    // Alpha and Beta
    benchmark_comparison_.beta = RiskMetrics::beta(strategy_returns, bench_returns);
    benchmark_comparison_.alpha = RiskMetrics::alpha(strategy_returns, bench_returns,
                                                      risk_free_rate_);

    // Correlation
    benchmark_comparison_.correlation = computeCorrelation(strategy_returns, bench_returns);

    // Tracking error and information ratio
    std::vector<double> excess_returns;
    for (size_t i = 0; i < strategy_returns.size(); ++i) {
        excess_returns.push_back(strategy_returns[i] - bench_returns[i]);
    }

    double excess_mean = computeMean(excess_returns);
    benchmark_comparison_.tracking_error = computeStdDev(excess_returns, excess_mean);

    if (benchmark_comparison_.tracking_error > 0) {
        benchmark_comparison_.information_ratio = excess_mean /
                                                   benchmark_comparison_.tracking_error;
    }

    // Treynor ratio
    if (std::abs(benchmark_comparison_.beta) > 1e-10) {
        double excess_return = metrics_.annualized_return - risk_free_rate_ * 100.0;
        benchmark_comparison_.treynor_ratio = excess_return / benchmark_comparison_.beta;
    }

    metrics_.information_ratio = benchmark_comparison_.information_ratio;
}

void PerformanceAnalyzer::setBenchmark(
    const std::string& name,
    const std::vector<std::pair<core::Timestamp, double>>& returns) {

    benchmark_comparison_.benchmark_name = name;
    benchmark_returns_ = returns;
    computed_ = false;
}

std::string PerformanceAnalyzer::generateReport(const ReportConfig& config) const {
    switch (config.format) {
        case ReportConfig::Format::Text:
            return generateTextReport(config);
        case ReportConfig::Format::JSON:
            return generateJSONReport(config);
        case ReportConfig::Format::CSV:
            return generateCSVReport(config);
        case ReportConfig::Format::HTML:
            return generateHTMLReport(config);
        default:
            return generateTextReport(config);
    }
}

void PerformanceAnalyzer::saveReport(const ReportConfig& config) const {
    std::string report = generateReport(config);

    if (!config.output_path.empty()) {
        std::ofstream file(config.output_path);
        if (file) {
            file << report;
        }
    }
}

std::string PerformanceAnalyzer::toJSON() const {
    ReportConfig config;
    config.format = ReportConfig::Format::JSON;
    return generateJSONReport(config);
}

double PerformanceAnalyzer::finalEquity() const {
    return equity_curve_.empty() ? initial_capital_ : equity_curve_.back().equity;
}

core::Timestamp PerformanceAnalyzer::startTime() const {
    return equity_curve_.empty() ? core::Timestamp::zero() : equity_curve_.front().timestamp;
}

core::Timestamp PerformanceAnalyzer::endTime() const {
    return equity_curve_.empty() ? core::Timestamp::zero() : equity_curve_.back().timestamp;
}

uint64_t PerformanceAnalyzer::durationDays() const {
    if (equity_curve_.size() < 2) return 0;

    uint64_t duration_us = (equity_curve_.back().timestamp.nanos -
                            equity_curve_.front().timestamp.nanos) / 1000;
    return duration_us / (24ULL * 60 * 60 * 1000000ULL);
}

// Statistical helpers
double PerformanceAnalyzer::computeMean(const std::vector<double>& values) const {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double PerformanceAnalyzer::computeStdDev(const std::vector<double>& values,
                                           double mean) const {
    if (values.size() < 2) return 0.0;

    double sum_sq = 0.0;
    for (double v : values) {
        sum_sq += (v - mean) * (v - mean);
    }
    return std::sqrt(sum_sq / (values.size() - 1));
}

double PerformanceAnalyzer::computeDownsideDeviation(const std::vector<double>& values,
                                                      double threshold) const {
    if (values.empty()) return 0.0;

    double sum_sq = 0.0;
    size_t count = 0;

    for (double v : values) {
        if (v < threshold) {
            sum_sq += (v - threshold) * (v - threshold);
            count++;
        }
    }

    return count > 0 ? std::sqrt(sum_sq / count) : 0.0;
}

double PerformanceAnalyzer::computeCorrelation(const std::vector<double>& x,
                                                const std::vector<double>& y) const {
    if (x.size() != y.size() || x.empty()) return 0.0;

    double mean_x = computeMean(x);
    double mean_y = computeMean(y);

    double cov = 0.0;
    double var_x = 0.0;
    double var_y = 0.0;

    for (size_t i = 0; i < x.size(); ++i) {
        double dx = x[i] - mean_x;
        double dy = y[i] - mean_y;
        cov += dx * dy;
        var_x += dx * dx;
        var_y += dy * dy;
    }

    if (var_x == 0 || var_y == 0) return 0.0;
    return cov / (std::sqrt(var_x) * std::sqrt(var_y));
}

double PerformanceAnalyzer::computePercentile(std::vector<double> values,
                                               double percentile) const {
    if (values.empty()) return 0.0;

    std::sort(values.begin(), values.end());
    size_t idx = static_cast<size_t>(percentile * (values.size() - 1));
    return values[idx];
}

std::string PerformanceAnalyzer::generateTextReport(const ReportConfig& config) const {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);

    ss << "================================================================================\n";
    ss << "                        BACKTEST PERFORMANCE REPORT\n";
    ss << "================================================================================\n\n";

    // Summary
    ss << "SUMMARY\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << "Initial Capital:      $" << std::setw(15) << initial_capital_ << "\n";
    ss << "Final Equity:         $" << std::setw(15) << finalEquity() << "\n";
    ss << "Total Return:         $" << std::setw(15) << metrics_.total_return;
    ss << " (" << metrics_.total_return_pct << "%)\n";
    ss << "Annualized Return:     " << std::setw(15) << metrics_.annualized_return << "%\n";
    ss << "Duration:              " << std::setw(15) << durationDays() << " days\n\n";

    // Risk Metrics
    ss << "RISK METRICS\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << "Volatility (Ann.):     " << std::setw(15) << metrics_.volatility << "%\n";
    ss << "Max Drawdown:          " << std::setw(15) << metrics_.max_drawdown_pct << "%\n";
    ss << "Average Drawdown:      " << std::setw(15) << metrics_.average_drawdown << "%\n";
    ss << "Sharpe Ratio:          " << std::setw(15) << metrics_.sharpe_ratio << "\n";
    ss << "Sortino Ratio:         " << std::setw(15) << metrics_.sortino_ratio << "\n";
    ss << "Calmar Ratio:          " << std::setw(15) << metrics_.calmar_ratio << "\n";
    ss << "Omega Ratio:           " << std::setw(15) << metrics_.omega_ratio << "\n\n";

    // Trade Statistics
    ss << "TRADE STATISTICS\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << "Total Trades:          " << std::setw(15) << metrics_.total_trades << "\n";
    ss << "Winning Trades:        " << std::setw(15) << metrics_.winning_trades << "\n";
    ss << "Losing Trades:         " << std::setw(15) << metrics_.losing_trades << "\n";
    ss << "Win Rate:              " << std::setw(15) << metrics_.win_rate << "%\n";
    ss << "Profit Factor:         " << std::setw(15) << metrics_.profit_factor << "\n";
    ss << "Expectancy:           $" << std::setw(15) << metrics_.expectancy << "\n";
    ss << "Average Win:          $" << std::setw(15) << metrics_.average_win << "\n";
    ss << "Average Loss:         $" << std::setw(15) << metrics_.average_loss << "\n";
    ss << "Largest Win:          $" << std::setw(15) << metrics_.largest_win << "\n";
    ss << "Largest Loss:         $" << std::setw(15) << metrics_.largest_loss << "\n";
    ss << "Payoff Ratio:          " << std::setw(15) << metrics_.payoff_ratio << "\n\n";

    // Duration Statistics
    ss << "DURATION STATISTICS\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << "Avg Trade Duration:    " << std::setw(15)
       << (metrics_.average_trade_duration_us / 1e6 / 60) << " min\n";
    ss << "Avg Winner Duration:   " << std::setw(15)
       << (metrics_.average_winner_duration_us / 1e6 / 60) << " min\n";
    ss << "Avg Loser Duration:    " << std::setw(15)
       << (metrics_.average_loser_duration_us / 1e6 / 60) << " min\n\n";

    // Streak Analysis
    ss << "STREAK ANALYSIS\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << "Max Consecutive Wins:  " << std::setw(15) << metrics_.max_consecutive_wins << "\n";
    ss << "Max Consecutive Losses:" << std::setw(15) << metrics_.max_consecutive_losses << "\n\n";

    // Costs
    ss << "COSTS\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << "Total Fees:           $" << std::setw(15) << metrics_.total_fees << "\n";
    ss << "Fees as % of P/L:      " << std::setw(15) << metrics_.fees_as_pct_of_pnl << "%\n";
    ss << "Total Volume:         $" << std::setw(15) << metrics_.total_volume << "\n\n";

    // Daily Stats
    ss << "DAILY STATISTICS\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << "Trades per Day:        " << std::setw(15) << metrics_.trades_per_day << "\n";
    ss << "Profitable Days:       " << std::setw(15) << metrics_.profitable_days << "\n";
    ss << "Losing Days:           " << std::setw(15) << metrics_.losing_days << "\n";
    ss << "Best Day:              " << std::setw(15) << metrics_.best_day << "%\n";
    ss << "Worst Day:             " << std::setw(15) << metrics_.worst_day << "%\n\n";

    // Benchmark comparison
    if (config.include_benchmark_comparison && !benchmark_comparison_.benchmark_name.empty()) {
        ss << "BENCHMARK COMPARISON (" << benchmark_comparison_.benchmark_name << ")\n";
        ss << "--------------------------------------------------------------------------------\n";
        ss << "Benchmark Return:      " << std::setw(15)
           << benchmark_comparison_.benchmark_return << "%\n";
        ss << "Alpha:                 " << std::setw(15)
           << benchmark_comparison_.alpha << "%\n";
        ss << "Beta:                  " << std::setw(15)
           << benchmark_comparison_.beta << "\n";
        ss << "Correlation:           " << std::setw(15)
           << benchmark_comparison_.correlation << "\n";
        ss << "Information Ratio:     " << std::setw(15)
           << benchmark_comparison_.information_ratio << "\n\n";
    }

    // Monthly returns table
    if (config.include_monthly_returns && !monthly_returns_.empty()) {
        ss << "MONTHLY RETURNS\n";
        ss << "--------------------------------------------------------------------------------\n";
        ss << std::setw(12) << "Period" << std::setw(12) << "Return %"
           << std::setw(10) << "Trades" << std::setw(12) << "Max DD %\n";

        for (const auto& mr : monthly_returns_) {
            uint64_t seconds = mr.start.to_seconds();
            std::time_t time = static_cast<std::time_t>(seconds);
            std::tm* tm = std::gmtime(&time);

            ss << std::setw(4) << (1900 + tm->tm_year) << "-"
               << std::setw(2) << std::setfill('0') << (tm->tm_mon + 1)
               << std::setfill(' ')
               << std::setw(7) << " "
               << std::setw(12) << mr.return_pct
               << std::setw(10) << mr.trades
               << std::setw(12) << mr.max_drawdown_pct << "\n";
        }
        ss << "\n";
    }

    ss << "================================================================================\n";

    return ss.str();
}

std::string PerformanceAnalyzer::generateJSONReport(const ReportConfig& config) const {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6);

    ss << "{\n";
    ss << "  \"summary\": {\n";
    ss << "    \"initial_capital\": " << initial_capital_ << ",\n";
    ss << "    \"final_equity\": " << finalEquity() << ",\n";
    ss << "    \"total_return\": " << metrics_.total_return << ",\n";
    ss << "    \"total_return_pct\": " << metrics_.total_return_pct << ",\n";
    ss << "    \"annualized_return\": " << metrics_.annualized_return << ",\n";
    ss << "    \"duration_days\": " << durationDays() << "\n";
    ss << "  },\n";

    ss << "  \"risk_metrics\": {\n";
    ss << "    \"volatility\": " << metrics_.volatility << ",\n";
    ss << "    \"max_drawdown\": " << metrics_.max_drawdown << ",\n";
    ss << "    \"max_drawdown_pct\": " << metrics_.max_drawdown_pct << ",\n";
    ss << "    \"average_drawdown\": " << metrics_.average_drawdown << ",\n";
    ss << "    \"sharpe_ratio\": " << metrics_.sharpe_ratio << ",\n";
    ss << "    \"sortino_ratio\": " << metrics_.sortino_ratio << ",\n";
    ss << "    \"calmar_ratio\": " << metrics_.calmar_ratio << ",\n";
    ss << "    \"omega_ratio\": " << metrics_.omega_ratio << ",\n";
    ss << "    \"ulcer_index\": " << metrics_.ulcer_index << "\n";
    ss << "  },\n";

    ss << "  \"trade_statistics\": {\n";
    ss << "    \"total_trades\": " << metrics_.total_trades << ",\n";
    ss << "    \"winning_trades\": " << metrics_.winning_trades << ",\n";
    ss << "    \"losing_trades\": " << metrics_.losing_trades << ",\n";
    ss << "    \"win_rate\": " << metrics_.win_rate << ",\n";
    ss << "    \"profit_factor\": " << metrics_.profit_factor << ",\n";
    ss << "    \"expectancy\": " << metrics_.expectancy << ",\n";
    ss << "    \"average_win\": " << metrics_.average_win << ",\n";
    ss << "    \"average_loss\": " << metrics_.average_loss << ",\n";
    ss << "    \"largest_win\": " << metrics_.largest_win << ",\n";
    ss << "    \"largest_loss\": " << metrics_.largest_loss << ",\n";
    ss << "    \"payoff_ratio\": " << metrics_.payoff_ratio << ",\n";
    ss << "    \"average_trade_duration_us\": " << metrics_.average_trade_duration_us << ",\n";
    ss << "    \"max_consecutive_wins\": " << metrics_.max_consecutive_wins << ",\n";
    ss << "    \"max_consecutive_losses\": " << metrics_.max_consecutive_losses << "\n";
    ss << "  },\n";

    ss << "  \"costs\": {\n";
    ss << "    \"total_fees\": " << metrics_.total_fees << ",\n";
    ss << "    \"total_volume\": " << metrics_.total_volume << ",\n";
    ss << "    \"fees_as_pct_of_pnl\": " << metrics_.fees_as_pct_of_pnl << "\n";
    ss << "  }";

    if (config.include_equity_curve && !equity_curve_.empty()) {
        ss << ",\n  \"equity_curve\": [\n";
        for (size_t i = 0; i < equity_curve_.size(); ++i) {
            const auto& point = equity_curve_[i];
            ss << "    {\"timestamp\": " << point.timestamp.nanos
               << ", \"equity\": " << point.equity
               << ", \"drawdown_pct\": " << point.drawdown_pct << "}";
            if (i < equity_curve_.size() - 1) ss << ",";
            ss << "\n";
        }
        ss << "  ]";
    }

    if (config.include_trade_list && !trades_.empty()) {
        ss << ",\n  \"trades\": [\n";
        for (size_t i = 0; i < trades_.size(); ++i) {
            const auto& t = trades_[i];
            ss << "    {\"id\": " << t.trade_id
               << ", \"symbol\": \"" << std::string(t.symbol.view()) << "\""
               << ", \"side\": \"" << (t.entry_side == core::Side::Buy ? "BUY" : "SELL") << "\""
               << ", \"entry_price\": " << t.entry_price.to_double()
               << ", \"exit_price\": " << t.exit_price.to_double()
               << ", \"quantity\": " << t.quantity.to_double()
               << ", \"pnl\": " << t.realized_pnl
               << ", \"duration_us\": " << t.duration_us << "}";
            if (i < trades_.size() - 1) ss << ",";
            ss << "\n";
        }
        ss << "  ]";
    }

    ss << "\n}\n";

    return ss.str();
}

std::string PerformanceAnalyzer::generateCSVReport(const ReportConfig& config) const {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6);

    // Metrics CSV
    ss << "Metric,Value\n";
    ss << "Initial Capital," << initial_capital_ << "\n";
    ss << "Final Equity," << finalEquity() << "\n";
    ss << "Total Return," << metrics_.total_return << "\n";
    ss << "Total Return %," << metrics_.total_return_pct << "\n";
    ss << "Annualized Return %," << metrics_.annualized_return << "\n";
    ss << "Volatility %," << metrics_.volatility << "\n";
    ss << "Max Drawdown %," << metrics_.max_drawdown_pct << "\n";
    ss << "Sharpe Ratio," << metrics_.sharpe_ratio << "\n";
    ss << "Sortino Ratio," << metrics_.sortino_ratio << "\n";
    ss << "Total Trades," << metrics_.total_trades << "\n";
    ss << "Win Rate %," << metrics_.win_rate << "\n";
    ss << "Profit Factor," << metrics_.profit_factor << "\n";

    return ss.str();
}

std::string PerformanceAnalyzer::generateHTMLReport(const ReportConfig& config) const {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);

    ss << "<!DOCTYPE html>\n<html>\n<head>\n";
    ss << "<title>Backtest Performance Report</title>\n";
    ss << "<style>\n";
    ss << "body { font-family: Arial, sans-serif; margin: 20px; }\n";
    ss << "table { border-collapse: collapse; width: 100%; margin: 20px 0; }\n";
    ss << "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }\n";
    ss << "th { background-color: #4CAF50; color: white; }\n";
    ss << "tr:nth-child(even) { background-color: #f2f2f2; }\n";
    ss << ".positive { color: green; }\n";
    ss << ".negative { color: red; }\n";
    ss << "</style>\n</head>\n<body>\n";

    ss << "<h1>Backtest Performance Report</h1>\n";

    ss << "<h2>Summary</h2>\n";
    ss << "<table>\n";
    ss << "<tr><th>Metric</th><th>Value</th></tr>\n";
    ss << "<tr><td>Initial Capital</td><td>$" << initial_capital_ << "</td></tr>\n";
    ss << "<tr><td>Final Equity</td><td>$" << finalEquity() << "</td></tr>\n";
    ss << "<tr><td>Total Return</td><td class='"
       << (metrics_.total_return >= 0 ? "positive" : "negative") << "'>"
       << metrics_.total_return_pct << "%</td></tr>\n";
    ss << "<tr><td>Sharpe Ratio</td><td>" << metrics_.sharpe_ratio << "</td></tr>\n";
    ss << "<tr><td>Max Drawdown</td><td class='negative'>"
       << metrics_.max_drawdown_pct << "%</td></tr>\n";
    ss << "</table>\n";

    ss << "</body>\n</html>\n";

    return ss.str();
}

// ============================================================================
// TradeTracker Implementation
// ============================================================================

void TradeTracker::processFill(const Fill& fill, double current_price) {
    std::string key(fill.symbol.view());

    auto it = open_positions_.find(key);

    if (it == open_positions_.end()) {
        // New position
        OpenPosition pos;
        pos.symbol = fill.symbol;
        pos.exchange = fill.exchange;
        pos.side = fill.side;
        pos.entry_time = fill.timestamp;
        pos.quantity = fill.quantity.to_double();
        pos.average_price = fill.price.to_double();
        pos.total_cost = fill.notional();
        pos.total_fees = fill.fee;
        pos.fill_count = 1;

        open_positions_[key] = pos;
    } else {
        auto& pos = it->second;

        if ((fill.side == core::Side::Buy && pos.side == core::Side::Buy) ||
            (fill.side == core::Side::Sell && pos.side == core::Side::Sell)) {
            // Adding to position
            double new_qty = fill.quantity.to_double();
            double old_cost = pos.average_price * pos.quantity;
            double new_cost = fill.price.to_double() * new_qty;

            pos.quantity += new_qty;
            pos.average_price = (old_cost + new_cost) / pos.quantity;
            pos.total_cost += fill.notional();
            pos.total_fees += fill.fee;
            pos.fill_count++;
        } else {
            // Closing position
            double close_qty = std::min(pos.quantity, fill.quantity.to_double());

            TradeRecord trade;
            trade.trade_id = next_trade_id_++;
            trade.symbol = pos.symbol;
            trade.exchange = pos.exchange;
            trade.entry_side = pos.side;
            trade.entry_time = pos.entry_time;
            trade.entry_price = core::Price::from_double(pos.average_price);
            trade.quantity = core::Quantity::from_double(close_qty);
            trade.exit_time = fill.timestamp;
            trade.exit_price = fill.price;
            trade.fees = pos.total_fees + fill.fee;
            trade.fills = pos.fill_count + 1;
            trade.duration_us = (fill.timestamp.nanos - pos.entry_time.nanos) / 1000;

            // Calculate P/L
            double entry_value = pos.average_price * close_qty;
            double exit_value = fill.price.to_double() * close_qty;

            if (pos.side == core::Side::Buy) {
                trade.realized_pnl = exit_value - entry_value - trade.fees;
            } else {
                trade.realized_pnl = entry_value - exit_value - trade.fees;
            }

            trade.realized_pnl_pct = (trade.realized_pnl / entry_value) * 100.0;

            completed_trades_.push_back(trade);

            // Update or close position
            pos.quantity -= close_qty;
            if (pos.quantity < 1e-10) {
                open_positions_.erase(key);
            } else {
                pos.total_fees = 0;  // Reset fees for remaining position
            }
        }
    }
}

std::vector<TradeRecord> TradeTracker::getCompletedTrades() {
    auto trades = std::move(completed_trades_);
    completed_trades_.clear();
    return trades;
}

bool TradeTracker::hasOpenPosition(const core::Symbol& symbol) const {
    return open_positions_.find(std::string(symbol.view())) != open_positions_.end();
}

double TradeTracker::getOpenQuantity(const core::Symbol& symbol) const {
    auto it = open_positions_.find(std::string(symbol.view()));
    return it != open_positions_.end() ? it->second.quantity : 0.0;
}

double TradeTracker::getAverageEntryPrice(const core::Symbol& symbol) const {
    auto it = open_positions_.find(std::string(symbol.view()));
    return it != open_positions_.end() ? it->second.average_price : 0.0;
}

void TradeTracker::clear() {
    open_positions_.clear();
    completed_trades_.clear();
}

// ============================================================================
// EquityCurveBuilder Implementation
// ============================================================================

EquityCurveBuilder::EquityCurveBuilder(double initial_capital)
    : initial_capital_(initial_capital)
    , peak_equity_(initial_capital) {
}

void EquityCurveBuilder::update(
    core::Timestamp timestamp, double portfolio_value,
    double realized_pnl, double unrealized_pnl,
    uint32_t open_positions, uint64_t trade_count) {

    EquityPoint point;
    point.timestamp = timestamp;
    point.equity = portfolio_value;
    point.realized_pnl = realized_pnl;
    point.unrealized_pnl = unrealized_pnl;
    point.open_positions = open_positions;
    point.trade_count = trade_count;

    if (portfolio_value > peak_equity_) {
        peak_equity_ = portfolio_value;
    }

    point.drawdown = peak_equity_ - portfolio_value;
    point.drawdown_pct = (peak_equity_ > 0) ?
        (point.drawdown / peak_equity_) * 100.0 : 0.0;

    points_.push_back(point);
}

std::vector<EquityPoint> EquityCurveBuilder::build() const {
    return points_;
}

std::vector<EquityPoint> EquityCurveBuilder::buildSampled(uint64_t interval_us) const {
    if (points_.empty()) return {};

    std::vector<EquityPoint> sampled;
    uint64_t next_sample_time = points_.front().timestamp.to_micros();

    for (const auto& point : points_) {
        uint64_t time_us = point.timestamp.to_micros();
        if (time_us >= next_sample_time) {
            sampled.push_back(point);
            next_sample_time = time_us + interval_us;
        }
    }

    // Always include last point
    if (!points_.empty() &&
        (sampled.empty() || sampled.back().timestamp != points_.back().timestamp)) {
        sampled.push_back(points_.back());
    }

    return sampled;
}

double EquityCurveBuilder::currentEquity() const {
    return points_.empty() ? initial_capital_ : points_.back().equity;
}

double EquityCurveBuilder::currentDrawdown() const {
    return points_.empty() ? 0.0 : points_.back().drawdown;
}

double EquityCurveBuilder::currentDrawdownPct() const {
    return points_.empty() ? 0.0 : points_.back().drawdown_pct;
}

double EquityCurveBuilder::peakEquity() const {
    return peak_equity_;
}

// ============================================================================
// RiskMetrics Implementation
// ============================================================================

double RiskMetrics::sharpeRatio(
    const std::vector<double>& returns,
    double risk_free_rate,
    double periods_per_year) {

    if (returns.empty()) return 0.0;

    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double rf_per_period = risk_free_rate / periods_per_year;
    double excess_return = mean - rf_per_period;

    double sum_sq = 0.0;
    for (double r : returns) {
        sum_sq += (r - mean) * (r - mean);
    }
    double std_dev = std::sqrt(sum_sq / (returns.size() - 1));

    if (std_dev == 0) return 0.0;

    return (excess_return / std_dev) * std::sqrt(periods_per_year);
}

double RiskMetrics::sortinoRatio(
    const std::vector<double>& returns,
    double target_return,
    double periods_per_year) {

    if (returns.empty()) return 0.0;

    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double excess = mean - target_return / periods_per_year;

    // Downside deviation
    double sum_sq = 0.0;
    size_t count = 0;
    for (double r : returns) {
        if (r < target_return / periods_per_year) {
            double diff = r - target_return / periods_per_year;
            sum_sq += diff * diff;
            count++;
        }
    }

    if (count == 0) return (excess > 0) ? std::numeric_limits<double>::infinity() : 0.0;

    double downside_dev = std::sqrt(sum_sq / count);
    if (downside_dev == 0) return 0.0;

    return (excess / downside_dev) * std::sqrt(periods_per_year);
}

double RiskMetrics::calmarRatio(double annualized_return, double max_drawdown) {
    if (max_drawdown == 0) return 0.0;
    return annualized_return / std::abs(max_drawdown);
}

double RiskMetrics::maxDrawdown(const std::vector<double>& equity_curve) {
    if (equity_curve.empty()) return 0.0;

    double peak = equity_curve[0];
    double max_dd = 0.0;

    for (double equity : equity_curve) {
        if (equity > peak) {
            peak = equity;
        }
        double dd = peak - equity;
        max_dd = std::max(max_dd, dd);
    }

    return max_dd;
}

double RiskMetrics::maxDrawdownPct(const std::vector<double>& equity_curve) {
    if (equity_curve.empty()) return 0.0;

    double peak = equity_curve[0];
    double max_dd_pct = 0.0;

    for (double equity : equity_curve) {
        if (equity > peak) {
            peak = equity;
        }
        if (peak > 0) {
            double dd_pct = (peak - equity) / peak;
            max_dd_pct = std::max(max_dd_pct, dd_pct);
        }
    }

    return max_dd_pct * 100.0;
}

double RiskMetrics::valueAtRisk(const std::vector<double>& returns, double confidence) {
    if (returns.empty()) return 0.0;

    std::vector<double> sorted = returns;
    std::sort(sorted.begin(), sorted.end());

    size_t idx = static_cast<size_t>((1.0 - confidence) * sorted.size());
    return -sorted[idx];
}

double RiskMetrics::conditionalVaR(const std::vector<double>& returns, double confidence) {
    if (returns.empty()) return 0.0;

    std::vector<double> sorted = returns;
    std::sort(sorted.begin(), sorted.end());

    size_t cutoff = static_cast<size_t>((1.0 - confidence) * sorted.size());
    if (cutoff == 0) cutoff = 1;

    double sum = 0.0;
    for (size_t i = 0; i < cutoff; ++i) {
        sum += sorted[i];
    }

    return -sum / cutoff;
}

double RiskMetrics::omegaRatio(const std::vector<double>& returns, double threshold) {
    if (returns.empty()) return 0.0;

    double gains = 0.0;
    double losses = 0.0;

    for (double r : returns) {
        if (r > threshold) {
            gains += r - threshold;
        } else {
            losses += threshold - r;
        }
    }

    if (losses == 0) return (gains > 0) ? std::numeric_limits<double>::infinity() : 0.0;
    return gains / losses;
}

double RiskMetrics::informationRatio(
    const std::vector<double>& strategy_returns,
    const std::vector<double>& benchmark_returns) {

    if (strategy_returns.size() != benchmark_returns.size() ||
        strategy_returns.empty()) return 0.0;

    std::vector<double> excess;
    for (size_t i = 0; i < strategy_returns.size(); ++i) {
        excess.push_back(strategy_returns[i] - benchmark_returns[i]);
    }

    double mean = std::accumulate(excess.begin(), excess.end(), 0.0) / excess.size();

    double sum_sq = 0.0;
    for (double e : excess) {
        sum_sq += (e - mean) * (e - mean);
    }
    double tracking_error = std::sqrt(sum_sq / (excess.size() - 1));

    if (tracking_error == 0) return 0.0;
    return mean / tracking_error;
}

double RiskMetrics::beta(
    const std::vector<double>& strategy_returns,
    const std::vector<double>& benchmark_returns) {

    if (strategy_returns.size() != benchmark_returns.size() ||
        strategy_returns.empty()) return 0.0;

    double mean_s = std::accumulate(strategy_returns.begin(),
                                     strategy_returns.end(), 0.0) / strategy_returns.size();
    double mean_b = std::accumulate(benchmark_returns.begin(),
                                     benchmark_returns.end(), 0.0) / benchmark_returns.size();

    double cov = 0.0;
    double var_b = 0.0;

    for (size_t i = 0; i < strategy_returns.size(); ++i) {
        double ds = strategy_returns[i] - mean_s;
        double db = benchmark_returns[i] - mean_b;
        cov += ds * db;
        var_b += db * db;
    }

    if (var_b == 0) return 0.0;
    return cov / var_b;
}

double RiskMetrics::alpha(
    const std::vector<double>& strategy_returns,
    const std::vector<double>& benchmark_returns,
    double risk_free_rate) {

    if (strategy_returns.size() != benchmark_returns.size() ||
        strategy_returns.empty()) return 0.0;

    double b = beta(strategy_returns, benchmark_returns);

    double mean_s = std::accumulate(strategy_returns.begin(),
                                     strategy_returns.end(), 0.0) / strategy_returns.size();
    double mean_b = std::accumulate(benchmark_returns.begin(),
                                     benchmark_returns.end(), 0.0) / benchmark_returns.size();

    // Assuming daily returns, annualize
    double rf_daily = risk_free_rate / 252.0;

    return (mean_s - rf_daily - b * (mean_b - rf_daily)) * 252.0;
}

double RiskMetrics::annualizedVolatility(
    const std::vector<double>& returns,
    double periods_per_year) {

    if (returns.size() < 2) return 0.0;

    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();

    double sum_sq = 0.0;
    for (double r : returns) {
        sum_sq += (r - mean) * (r - mean);
    }

    double std_dev = std::sqrt(sum_sq / (returns.size() - 1));
    return std_dev * std::sqrt(periods_per_year);
}

double RiskMetrics::ulcerIndex(const std::vector<double>& prices) {
    if (prices.empty()) return 0.0;

    double peak = prices[0];
    double sum_sq = 0.0;

    for (double price : prices) {
        if (price > peak) {
            peak = price;
        }
        if (peak > 0) {
            double dd_pct = ((peak - price) / peak) * 100.0;
            sum_sq += dd_pct * dd_pct;
        }
    }

    return std::sqrt(sum_sq / prices.size());
}

}  // namespace hft::backtesting
