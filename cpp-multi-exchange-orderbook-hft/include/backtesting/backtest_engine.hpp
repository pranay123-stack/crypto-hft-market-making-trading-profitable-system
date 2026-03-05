#pragma once

/**
 * @file backtest_engine.hpp
 * @brief Main backtesting engine orchestrator
 *
 * Provides the central orchestration for backtesting strategies
 * with historical data, supporting multi-exchange simulation
 * with configurable parameters.
 */

#include "core/types.hpp"
#include "backtesting/data_feed.hpp"
#include "backtesting/simulated_exchange.hpp"
#include "backtesting/performance_analyzer.hpp"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <optional>

namespace hft::backtesting {

// Forward declarations
class Strategy;

// ============================================================================
// Strategy Interface
// ============================================================================

/**
 * @brief Abstract base class for trading strategies
 *
 * Strategies must implement this interface to be used with the backtest engine.
 */
class Strategy {
public:
    virtual ~Strategy() = default;

    // Identification
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string version() const { return "1.0.0"; }

    // Lifecycle
    virtual void initialize(const std::unordered_map<std::string, std::string>& params) {}
    virtual void shutdown() {}

    // Event handlers
    virtual void onTrade(const Trade& trade) {}
    virtual void onOrderBook(const OrderBookSnapshot& snapshot) {}
    virtual void onOrderBookUpdate(const OrderBookUpdate& update) {}
    virtual void onBar(const OHLCVBar& bar) {}

    // Order management callbacks
    virtual void onOrderUpdate(const SimulatedOrder& order) {}
    virtual void onFill(const Fill& fill) {}
    virtual void onPositionUpdate(const Position& position) {}

    // Periodic callbacks
    virtual void onTick(core::Timestamp timestamp) {}  // Called each simulation tick
    virtual void onMinute(core::Timestamp timestamp) {}
    virtual void onHour(core::Timestamp timestamp) {}
    virtual void onDay(core::Timestamp timestamp) {}

    // Exchange access (set by engine)
    void setExchange(SimulatedExchange* exchange) { exchange_ = exchange; }
    [[nodiscard]] SimulatedExchange* exchange() const { return exchange_; }

    // Current time access
    void setCurrentTime(core::Timestamp time) { current_time_ = time; }
    [[nodiscard]] core::Timestamp currentTime() const { return current_time_; }

protected:
    SimulatedExchange* exchange_{nullptr};
    core::Timestamp current_time_;
};

// ============================================================================
// Backtest Configuration
// ============================================================================

/**
 * @brief Progress callback type
 */
using ProgressCallback = std::function<void(
    double progress,           // 0.0 to 1.0
    core::Timestamp current,   // Current simulation time
    uint64_t events_processed, // Events processed
    uint64_t trades_executed   // Trades executed
)>;

/**
 * @brief Backtest engine configuration
 */
struct BacktestConfig {
    // Time range
    core::Timestamp start_time;
    core::Timestamp end_time;

    // Initial capital per exchange
    std::unordered_map<std::string, std::unordered_map<std::string, double>> initial_balances;
    // e.g., {"binance": {"USDT": 100000, "BTC": 1.0}, "coinbase": {"USDT": 50000}}

    // Default initial capital (if not specified per exchange)
    double default_initial_capital{100000.0};
    std::string default_quote_currency{"USDT"};

    // Simulation settings
    uint64_t tick_interval_us{1000};     // Minimum tick interval (1ms default)
    bool process_every_event{true};       // Process each event vs. batch processing
    uint64_t batch_interval_us{0};        // If not processing every event

    // Progress reporting
    ProgressCallback progress_callback;
    uint64_t progress_interval_events{10000};  // Report progress every N events
    uint64_t progress_interval_us{1000000};    // Or every N microseconds

    // Performance settings
    bool enable_warmup{true};
    uint64_t warmup_period_us{0};         // Warm-up period (no trading)

    // Exchange configurations per exchange
    std::unordered_map<core::Exchange, SimulatedExchangeConfig> exchange_configs;

    // Strategy parameters
    std::unordered_map<std::string, std::string> strategy_params;

    // Output settings
    bool save_trades{true};
    bool save_equity_curve{true};
    uint64_t equity_sample_interval_us{60000000};  // 1 minute default
    std::string output_directory;

    // Logging
    bool verbose{false};
    bool log_orders{false};
    bool log_fills{true};

    // Risk-free rate for performance calculation
    double risk_free_rate{0.02};
};

// ============================================================================
// Backtest State
// ============================================================================

/**
 * @brief Current state of backtest
 */
enum class BacktestState {
    Idle,
    Initializing,
    Running,
    Paused,
    Completed,
    Failed,
    Cancelled
};

[[nodiscard]] constexpr std::string_view backtest_state_to_string(BacktestState state) noexcept {
    switch (state) {
        case BacktestState::Idle:        return "IDLE";
        case BacktestState::Initializing: return "INITIALIZING";
        case BacktestState::Running:     return "RUNNING";
        case BacktestState::Paused:      return "PAUSED";
        case BacktestState::Completed:   return "COMPLETED";
        case BacktestState::Failed:      return "FAILED";
        case BacktestState::Cancelled:   return "CANCELLED";
    }
    return "UNKNOWN";
}

// ============================================================================
// Backtest Result
// ============================================================================

/**
 * @brief Complete backtest result
 */
struct BacktestResult {
    // Status
    BacktestState state{BacktestState::Idle};
    std::string error_message;

    // Time info
    core::Timestamp actual_start_time;
    core::Timestamp actual_end_time;
    uint64_t duration_us{0};               // Backtest duration (simulation time)
    uint64_t execution_time_ms{0};         // Real wall-clock execution time

    // Event statistics
    uint64_t total_events{0};
    uint64_t events_processed{0};
    uint64_t trades_executed{0};
    uint64_t orders_submitted{0};
    uint64_t orders_filled{0};
    uint64_t orders_cancelled{0};
    uint64_t orders_rejected{0};

    // Performance metrics
    PerformanceMetrics metrics;

    // Detailed data
    std::vector<TradeRecord> trades;
    std::vector<EquityPoint> equity_curve;
    std::vector<DrawdownPeriod> drawdowns;
    std::vector<PeriodReturn> monthly_returns;

    // Per-exchange results
    struct ExchangeResult {
        core::Exchange exchange;
        double final_equity{0.0};
        double total_return{0.0};
        uint64_t trades{0};
        double volume{0.0};
        double fees{0.0};
    };
    std::vector<ExchangeResult> exchange_results;

    // Per-symbol results
    struct SymbolResult {
        core::Symbol symbol;
        core::Exchange exchange;
        uint64_t trades{0};
        double pnl{0.0};
        double volume{0.0};
        double win_rate{0.0};
    };
    std::vector<SymbolResult> symbol_results;

    // Summary stats
    [[nodiscard]] double eventsPerSecond() const noexcept {
        return execution_time_ms > 0 ?
            static_cast<double>(events_processed) / (execution_time_ms / 1000.0) : 0.0;
    }

    [[nodiscard]] bool isSuccessful() const noexcept {
        return state == BacktestState::Completed;
    }
};

// ============================================================================
// Main Backtest Engine
// ============================================================================

/**
 * @brief Main backtesting engine
 *
 * Orchestrates the entire backtesting process including:
 * - Data feed management
 * - Strategy execution
 * - Exchange simulation
 * - Performance analysis
 * - Progress reporting
 */
class BacktestEngine {
public:
    BacktestEngine();
    ~BacktestEngine();

    // Non-copyable
    BacktestEngine(const BacktestEngine&) = delete;
    BacktestEngine& operator=(const BacktestEngine&) = delete;

    // Configuration
    void setConfig(const BacktestConfig& config);
    [[nodiscard]] const BacktestConfig& config() const { return config_; }

    // Data feed
    void setDataFeed(std::shared_ptr<DataFeed> feed);
    void addDataFeed(core::Exchange exchange, std::shared_ptr<DataFeed> feed);

    // Strategy
    void setStrategy(std::shared_ptr<Strategy> strategy);

    // Exchange configuration
    void setExchangeConfig(core::Exchange exchange, const SimulatedExchangeConfig& config);
    [[nodiscard]] SimulatedExchange* getExchange(core::Exchange exchange) const;

    // Run backtest
    [[nodiscard]] BacktestResult run();

    // Async execution
    void runAsync();
    [[nodiscard]] bool isRunning() const { return state_.load() == BacktestState::Running; }
    [[nodiscard]] BacktestResult getResult() const;

    // Control
    void pause();
    void resume();
    void cancel();

    // State queries
    [[nodiscard]] BacktestState state() const { return state_.load(); }
    [[nodiscard]] core::Timestamp currentTime() const { return current_time_; }
    [[nodiscard]] double progress() const;
    [[nodiscard]] uint64_t eventsProcessed() const { return events_processed_.load(); }

    // Performance analyzer access
    [[nodiscard]] const PerformanceAnalyzer& analyzer() const { return analyzer_; }

private:
    // Initialization
    bool initialize();
    void cleanup();

    // Event processing
    void processEvent(const DataEvent& event);
    void processTick(core::Timestamp timestamp);
    void processPeriodicCallbacks(core::Timestamp timestamp);

    // Order/fill handling
    void onOrderUpdate(const SimulatedOrder& order);
    void onFill(const Fill& fill);

    // Time management
    void advanceTime(core::Timestamp new_time);
    [[nodiscard]] bool shouldProcessPeriodicCallback(
        core::Timestamp& last_callback,
        core::Timestamp current,
        uint64_t interval_us) const;

    // Progress reporting
    void reportProgress();

    // Result building
    void buildResult();
    void buildExchangeResults();
    void buildSymbolResults();

    // Configuration
    BacktestConfig config_;

    // Components
    std::shared_ptr<DataFeed> primary_feed_;
    std::unordered_map<core::Exchange, std::shared_ptr<DataFeed>> exchange_feeds_;
    std::unique_ptr<MultiExchangeDataSynchronizer> data_synchronizer_;

    std::shared_ptr<Strategy> strategy_;
    std::unordered_map<core::Exchange, std::unique_ptr<SimulatedExchange>> exchanges_;

    PerformanceAnalyzer analyzer_;
    TradeTracker trade_tracker_;
    EquityCurveBuilder equity_builder_;

    // State
    std::atomic<BacktestState> state_{BacktestState::Idle};
    core::Timestamp current_time_;
    core::Timestamp last_minute_callback_;
    core::Timestamp last_hour_callback_;
    core::Timestamp last_day_callback_;
    core::Timestamp last_progress_report_;
    core::Timestamp last_equity_sample_;

    // Statistics
    std::atomic<uint64_t> events_processed_{0};
    std::atomic<uint64_t> trades_executed_{0};
    std::atomic<uint64_t> orders_submitted_{0};
    std::atomic<uint64_t> orders_filled_{0};
    std::atomic<uint64_t> orders_cancelled_{0};
    std::atomic<uint64_t> orders_rejected_{0};

    // Timing
    std::chrono::steady_clock::time_point start_wall_time_;
    std::chrono::steady_clock::time_point end_wall_time_;

    // Result
    BacktestResult result_;

    // Threading
    std::unique_ptr<std::thread> async_thread_;
    std::mutex mutex_;
    std::condition_variable pause_cv_;
    std::atomic<bool> should_pause_{false};
    std::atomic<bool> should_cancel_{false};

    // Warm-up tracking
    bool in_warmup_{false};
};

// ============================================================================
// Backtest Runner (Convenience class for running multiple backtests)
// ============================================================================

/**
 * @brief Runs multiple backtests with parameter variations
 */
class BacktestRunner {
public:
    struct ParameterSet {
        std::string name;
        std::unordered_map<std::string, std::string> params;
    };

    BacktestRunner() = default;

    // Configure base settings
    void setBaseConfig(const BacktestConfig& config);
    void setDataFeed(std::shared_ptr<DataFeed> feed);

    // Add strategy factory
    using StrategyFactory = std::function<std::shared_ptr<Strategy>()>;
    void setStrategyFactory(StrategyFactory factory);

    // Add parameter sets to test
    void addParameterSet(const std::string& name,
                         const std::unordered_map<std::string, std::string>& params);

    // Generate parameter grid
    void generateParameterGrid(
        const std::string& param_name,
        const std::vector<std::string>& values);

    // Run all variations
    std::vector<BacktestResult> runAll();

    // Run in parallel
    std::vector<BacktestResult> runAllParallel(size_t max_threads = 0);

    // Progress callback
    using RunnerProgressCallback = std::function<void(
        size_t completed, size_t total, const std::string& current_name)>;
    void setProgressCallback(RunnerProgressCallback callback);

private:
    BacktestConfig base_config_;
    std::shared_ptr<DataFeed> feed_;
    StrategyFactory strategy_factory_;
    std::vector<ParameterSet> parameter_sets_;
    RunnerProgressCallback progress_callback_;
};

// ============================================================================
// Walk-Forward Optimization
// ============================================================================

/**
 * @brief Walk-forward analysis configuration
 */
struct WalkForwardConfig {
    core::Timestamp total_start;
    core::Timestamp total_end;

    // Window sizes
    uint64_t in_sample_days{60};         // Training window
    uint64_t out_of_sample_days{20};     // Testing window
    uint64_t step_days{20};              // Step size

    // Optimization settings
    std::string optimization_metric{"sharpe_ratio"};  // Metric to optimize
    bool maximize{true};                               // Maximize or minimize

    // Parameter ranges for optimization
    struct ParameterRange {
        std::string name;
        std::vector<std::string> values;
    };
    std::vector<ParameterRange> parameters;
};

/**
 * @brief Walk-forward analysis result
 */
struct WalkForwardResult {
    struct Window {
        core::Timestamp in_sample_start;
        core::Timestamp in_sample_end;
        core::Timestamp out_of_sample_start;
        core::Timestamp out_of_sample_end;

        std::unordered_map<std::string, std::string> best_params;
        double in_sample_metric{0.0};
        double out_of_sample_metric{0.0};

        PerformanceMetrics in_sample_metrics;
        PerformanceMetrics out_of_sample_metrics;
    };

    std::vector<Window> windows;

    // Aggregated out-of-sample results
    PerformanceMetrics aggregated_metrics;
    double average_out_of_sample_metric{0.0};
    double consistency_ratio{0.0};  // % of windows where OOS > 0

    // Robustness metrics
    double parameter_stability{0.0};  // How stable are optimal parameters
};

/**
 * @brief Walk-forward optimizer
 */
class WalkForwardOptimizer {
public:
    WalkForwardOptimizer() = default;

    void setConfig(const WalkForwardConfig& config);
    void setDataFeed(std::shared_ptr<DataFeed> feed);
    void setStrategyFactory(std::function<std::shared_ptr<Strategy>()> factory);

    [[nodiscard]] WalkForwardResult run();

    // Progress callback
    using WFProgressCallback = std::function<void(
        size_t window_completed, size_t total_windows,
        const std::string& status)>;
    void setProgressCallback(WFProgressCallback callback);

private:
    WalkForwardConfig config_;
    std::shared_ptr<DataFeed> feed_;
    std::function<std::shared_ptr<Strategy>()> strategy_factory_;
    WFProgressCallback progress_callback_;

    [[nodiscard]] std::vector<std::pair<core::Timestamp, core::Timestamp>>
        generateWindows() const;

    [[nodiscard]] double extractMetric(
        const PerformanceMetrics& metrics,
        const std::string& metric_name) const;
};

}  // namespace hft::backtesting
