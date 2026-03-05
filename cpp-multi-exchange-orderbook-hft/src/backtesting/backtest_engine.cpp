/**
 * @file backtest_engine.cpp
 * @brief Main backtesting engine implementation
 */

#include "backtesting/backtest_engine.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace hft::backtesting {

// ============================================================================
// BacktestEngine Implementation
// ============================================================================

BacktestEngine::BacktestEngine()
    : analyzer_(100000.0)
    , equity_builder_(100000.0) {
}

BacktestEngine::~BacktestEngine() {
    if (async_thread_ && async_thread_->joinable()) {
        should_cancel_.store(true);
        pause_cv_.notify_all();
        async_thread_->join();
    }
}

void BacktestEngine::setConfig(const BacktestConfig& config) {
    config_ = config;
    analyzer_.setRiskFreeRate(config.risk_free_rate);
    analyzer_.reset(config.default_initial_capital);
    equity_builder_ = EquityCurveBuilder(config.default_initial_capital);
}

void BacktestEngine::setDataFeed(std::shared_ptr<DataFeed> feed) {
    primary_feed_ = std::move(feed);
}

void BacktestEngine::addDataFeed(core::Exchange exchange, std::shared_ptr<DataFeed> feed) {
    exchange_feeds_[exchange] = std::move(feed);
}

void BacktestEngine::setStrategy(std::shared_ptr<Strategy> strategy) {
    strategy_ = std::move(strategy);
}

void BacktestEngine::setExchangeConfig(core::Exchange exchange,
                                         const SimulatedExchangeConfig& config) {
    config_.exchange_configs[exchange] = config;
}

SimulatedExchange* BacktestEngine::getExchange(core::Exchange exchange) const {
    auto it = exchanges_.find(exchange);
    return it != exchanges_.end() ? it->second.get() : nullptr;
}

bool BacktestEngine::initialize() {
    if (!strategy_) {
        result_.error_message = "No strategy set";
        return false;
    }

    if (!primary_feed_ && exchange_feeds_.empty()) {
        result_.error_message = "No data feed set";
        return false;
    }

    // Initialize data synchronizer if multiple feeds
    if (!exchange_feeds_.empty()) {
        MultiExchangeDataSynchronizer::SyncConfig sync_config;
        sync_config.max_clock_skew_us = 1000;
        sync_config.sync_window_us = 100;

        data_synchronizer_ = std::make_unique<MultiExchangeDataSynchronizer>(sync_config);

        for (auto& [exchange, feed] : exchange_feeds_) {
            if (!feed->initialize()) {
                result_.error_message = "Failed to initialize data feed for " +
                    std::string(core::exchange_to_string(exchange));
                return false;
            }
            data_synchronizer_->addFeed(exchange, feed);
        }
    } else if (primary_feed_) {
        if (!primary_feed_->initialize()) {
            result_.error_message = "Failed to initialize data feed";
            return false;
        }
    }

    // Create simulated exchanges
    // Collect all exchanges from data feeds
    std::set<core::Exchange> all_exchanges;

    if (primary_feed_) {
        for (auto ex : primary_feed_->exchanges()) {
            all_exchanges.insert(ex);
        }
    }

    for (const auto& [ex, feed] : exchange_feeds_) {
        all_exchanges.insert(ex);
    }

    // Default to Binance if no exchanges detected
    if (all_exchanges.empty()) {
        all_exchanges.insert(core::Exchange::Binance);
    }

    // Create exchange instances
    for (core::Exchange exchange : all_exchanges) {
        SimulatedExchangeConfig ex_config;

        // Check for custom config
        auto config_it = config_.exchange_configs.find(exchange);
        if (config_it != config_.exchange_configs.end()) {
            ex_config = config_it->second;
        } else {
            ex_config = SimulatedExchangeFactory::getDefaultConfig(exchange);
        }

        auto sim_exchange = std::make_unique<SimulatedExchange>(ex_config);

        // Initialize with balances
        std::unordered_map<std::string, double> balances;

        auto balance_it = config_.initial_balances.find(std::string(
            core::exchange_to_string(exchange)));
        if (balance_it != config_.initial_balances.end()) {
            balances = balance_it->second;
        } else {
            balances[config_.default_quote_currency] = config_.default_initial_capital;
        }

        sim_exchange->initialize(balances);

        // Set callbacks
        sim_exchange->setOnOrder([this](const SimulatedOrder& order) {
            onOrderUpdate(order);
        });

        sim_exchange->setOnFill([this](const Fill& fill) {
            onFill(fill);
        });

        exchanges_[exchange] = std::move(sim_exchange);
    }

    // Initialize strategy
    if (!exchanges_.empty()) {
        strategy_->setExchange(exchanges_.begin()->second.get());
    }

    strategy_->initialize(config_.strategy_params);

    // Set initial time
    if (config_.start_time.nanos > 0) {
        current_time_ = config_.start_time;
    } else if (primary_feed_) {
        current_time_ = primary_feed_->startTime();
    }

    // Initialize warm-up state
    in_warmup_ = config_.enable_warmup && config_.warmup_period_us > 0;

    // Reset callbacks timestamps
    last_minute_callback_ = current_time_;
    last_hour_callback_ = current_time_;
    last_day_callback_ = current_time_;
    last_progress_report_ = current_time_;
    last_equity_sample_ = current_time_;

    // Reset statistics
    events_processed_ = 0;
    trades_executed_ = 0;
    orders_submitted_ = 0;
    orders_filled_ = 0;
    orders_cancelled_ = 0;
    orders_rejected_ = 0;

    // Clear result
    result_ = BacktestResult{};
    result_.actual_start_time = current_time_;

    return true;
}

void BacktestEngine::cleanup() {
    if (strategy_) {
        strategy_->shutdown();
    }

    exchanges_.clear();
    data_synchronizer_.reset();
}

BacktestResult BacktestEngine::run() {
    state_.store(BacktestState::Initializing);

    start_wall_time_ = std::chrono::steady_clock::now();

    if (!initialize()) {
        state_.store(BacktestState::Failed);
        return result_;
    }

    state_.store(BacktestState::Running);

    try {
        // Main simulation loop
        if (data_synchronizer_) {
            // Multi-exchange synchronized iteration
            while (data_synchronizer_->hasNext() && !should_cancel_.load()) {
                // Handle pause
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    pause_cv_.wait(lock, [this] {
                        return !should_pause_.load() || should_cancel_.load();
                    });
                }

                if (should_cancel_.load()) break;

                auto event = data_synchronizer_->next();
                if (event) {
                    processEvent(*event);
                }
            }
        } else if (primary_feed_) {
            // Single feed iteration
            while (primary_feed_->hasNext() && !should_cancel_.load()) {
                // Handle pause
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    pause_cv_.wait(lock, [this] {
                        return !should_pause_.load() || should_cancel_.load();
                    });
                }

                if (should_cancel_.load()) break;

                auto event = primary_feed_->next();
                if (event) {
                    processEvent(*event);
                }
            }
        }

        if (should_cancel_.load()) {
            state_.store(BacktestState::Cancelled);
        } else {
            state_.store(BacktestState::Completed);
        }

    } catch (const std::exception& e) {
        result_.error_message = e.what();
        state_.store(BacktestState::Failed);
    }

    end_wall_time_ = std::chrono::steady_clock::now();

    buildResult();
    cleanup();

    return result_;
}

void BacktestEngine::runAsync() {
    async_thread_ = std::make_unique<std::thread>([this]() {
        run();
    });
}

BacktestResult BacktestEngine::getResult() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

void BacktestEngine::pause() {
    should_pause_.store(true);
    state_.store(BacktestState::Paused);
}

void BacktestEngine::resume() {
    should_pause_.store(false);
    state_.store(BacktestState::Running);
    pause_cv_.notify_all();
}

void BacktestEngine::cancel() {
    should_cancel_.store(true);
    pause_cv_.notify_all();
}

double BacktestEngine::progress() const {
    if (primary_feed_) {
        return primary_feed_->progress();
    }
    return 0.0;
}

void BacktestEngine::processEvent(const DataEvent& event) {
    // Check time range
    if (config_.end_time.nanos > 0 && event.timestamp > config_.end_time) {
        return;
    }

    // Advance simulation time
    advanceTime(event.timestamp);

    // Find the appropriate exchange
    SimulatedExchange* exchange = nullptr;
    auto ex_it = exchanges_.find(event.exchange);
    if (ex_it != exchanges_.end()) {
        exchange = ex_it->second.get();
    } else if (!exchanges_.empty()) {
        exchange = exchanges_.begin()->second.get();
    }

    // Update exchange time
    if (exchange) {
        exchange->setCurrentTime(current_time_);
    }

    // Update strategy time and exchange
    strategy_->setCurrentTime(current_time_);
    if (exchange) {
        strategy_->setExchange(exchange);
    }

    // Process event based on type
    switch (event.type) {
        case DataEventType::Trade:
            if (event.data.trade.trade) {
                const auto& trade = *event.data.trade.trade;

                // Update exchange with trade
                if (exchange) {
                    exchange->onTrade(trade);
                }

                // Notify strategy if not in warm-up
                if (!in_warmup_) {
                    strategy_->onTrade(trade);
                }
            }
            break;

        case DataEventType::OrderBookSnapshot:
            if (event.data.order_book.snapshot) {
                const auto& snapshot = *event.data.order_book.snapshot;

                if (exchange) {
                    exchange->onOrderBookSnapshot(snapshot);
                }

                if (!in_warmup_) {
                    strategy_->onOrderBook(snapshot);
                }
            }
            break;

        case DataEventType::OrderBookUpdate:
            if (event.data.book_update.update) {
                const auto& update = *event.data.book_update.update;

                if (exchange) {
                    exchange->onOrderBookUpdate(update);
                }

                if (!in_warmup_) {
                    strategy_->onOrderBookUpdate(update);
                }
            }
            break;

        case DataEventType::OHLCVBar:
            if (event.data.ohlcv.bar) {
                const auto& bar = *event.data.ohlcv.bar;

                if (!in_warmup_) {
                    strategy_->onBar(bar);
                }
            }
            break;
    }

    // Call tick handler
    processTick(current_time_);

    events_processed_++;

    // Report progress periodically
    if (events_processed_ % config_.progress_interval_events == 0) {
        reportProgress();
    }
}

void BacktestEngine::processTick(core::Timestamp timestamp) {
    // Process pending exchange events
    for (auto& [exchange, sim_exchange] : exchanges_) {
        sim_exchange->processEvents();
    }

    // Call strategy tick handler
    if (!in_warmup_) {
        strategy_->onTick(timestamp);
    }

    // Process periodic callbacks
    processPeriodicCallbacks(timestamp);

    // Sample equity curve
    if (shouldProcessPeriodicCallback(last_equity_sample_, timestamp,
                                       config_.equity_sample_interval_us)) {
        double total_equity = 0.0;
        double unrealized_pnl = 0.0;
        uint32_t open_positions = 0;

        for (const auto& [exchange, sim_exchange] : exchanges_) {
            total_equity += sim_exchange->getTotalEquity(config_.default_quote_currency);

            for (const auto& pos : sim_exchange->getAllPositions()) {
                unrealized_pnl += pos.unrealized_pnl;
                if (!pos.is_flat()) {
                    open_positions++;
                }
            }
        }

        equity_builder_.update(timestamp, total_equity, 0.0, unrealized_pnl,
                               open_positions, trades_executed_);
    }
}

void BacktestEngine::processPeriodicCallbacks(core::Timestamp timestamp) {
    // Check warm-up period
    if (in_warmup_) {
        if (timestamp.nanos >= result_.actual_start_time.nanos + config_.warmup_period_us * 1000) {
            in_warmup_ = false;
            if (config_.verbose) {
                std::cout << "Warm-up period completed at " << timestamp.nanos << std::endl;
            }
        }
        return;
    }

    // Minute callback
    if (shouldProcessPeriodicCallback(last_minute_callback_, timestamp, 60'000'000)) {
        strategy_->onMinute(timestamp);
    }

    // Hour callback
    if (shouldProcessPeriodicCallback(last_hour_callback_, timestamp, 3600'000'000ULL)) {
        strategy_->onHour(timestamp);
    }

    // Day callback
    if (shouldProcessPeriodicCallback(last_day_callback_, timestamp, 86400'000'000ULL)) {
        strategy_->onDay(timestamp);

        // Record daily equity for analyzer
        double total_equity = 0.0;
        for (const auto& [exchange, sim_exchange] : exchanges_) {
            total_equity += sim_exchange->getTotalEquity(config_.default_quote_currency);
        }
        analyzer_.recordDailyClose(timestamp, total_equity);
    }
}

bool BacktestEngine::shouldProcessPeriodicCallback(
    core::Timestamp& last_callback,
    core::Timestamp current,
    uint64_t interval_us) const {

    uint64_t interval_ns = interval_us * 1000ULL;

    if (current.nanos >= last_callback.nanos + interval_ns) {
        last_callback = current;
        return true;
    }
    return false;
}

void BacktestEngine::advanceTime(core::Timestamp new_time) {
    if (new_time > current_time_) {
        current_time_ = new_time;
    }
}

void BacktestEngine::onOrderUpdate(const SimulatedOrder& order) {
    if (in_warmup_) return;

    strategy_->onOrderUpdate(order);

    // Track statistics
    switch (order.status) {
        case core::OrderStatus::New:
            orders_submitted_++;
            break;
        case core::OrderStatus::Filled:
            orders_filled_++;
            break;
        case core::OrderStatus::Cancelled:
            orders_cancelled_++;
            break;
        case core::OrderStatus::Rejected:
            orders_rejected_++;
            break;
        default:
            break;
    }

    if (config_.log_orders && config_.verbose) {
        std::cout << "[ORDER] " << std::string(core::order_status_to_string(order.status))
                  << " " << std::string(order.symbol.view())
                  << " " << std::string(core::side_to_string(order.side))
                  << " @ " << order.price.to_double()
                  << " qty=" << order.remaining_quantity.to_double()
                  << std::endl;
    }
}

void BacktestEngine::onFill(const Fill& fill) {
    if (in_warmup_) return;

    // Record fill
    analyzer_.recordFill(fill);

    // Track trade
    double current_price = fill.price.to_double();
    trade_tracker_.processFill(fill, current_price);

    // Get completed trades
    auto completed = trade_tracker_.getCompletedTrades();
    for (const auto& trade : completed) {
        analyzer_.recordTradeClose(trade);
        trades_executed_++;
    }

    // Notify strategy
    strategy_->onFill(fill);

    if (config_.log_fills && config_.verbose) {
        std::cout << "[FILL] " << std::string(fill.symbol.view())
                  << " " << std::string(core::side_to_string(fill.side))
                  << " " << fill.quantity.to_double()
                  << " @ " << fill.price.to_double()
                  << " fee=" << fill.fee
                  << std::endl;
    }
}

void BacktestEngine::reportProgress() {
    if (!config_.progress_callback) return;

    double prog = progress();
    config_.progress_callback(prog, current_time_, events_processed_, trades_executed_);
}

void BacktestEngine::buildResult() {
    result_.state = state_.load();
    result_.actual_end_time = current_time_;

    // Calculate durations
    result_.duration_us = (current_time_.nanos - result_.actual_start_time.nanos) / 1000;
    result_.execution_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_wall_time_ - start_wall_time_).count();

    // Copy statistics
    result_.total_events = primary_feed_ ? primary_feed_->totalEvents() : 0;
    result_.events_processed = events_processed_.load();
    result_.trades_executed = trades_executed_.load();
    result_.orders_submitted = orders_submitted_.load();
    result_.orders_filled = orders_filled_.load();
    result_.orders_cancelled = orders_cancelled_.load();
    result_.orders_rejected = orders_rejected_.load();

    // Build equity curve from builder
    auto equity_points = equity_builder_.build();
    for (const auto& point : equity_points) {
        analyzer_.recordEquityPoint(point);
    }

    // Compute performance metrics
    analyzer_.compute();

    result_.metrics = analyzer_.metrics();
    result_.trades = analyzer_.trades();
    result_.equity_curve = analyzer_.equityCurve();
    result_.drawdowns = analyzer_.drawdowns();
    result_.monthly_returns = analyzer_.monthlyReturns();

    // Build per-exchange results
    buildExchangeResults();

    // Build per-symbol results
    buildSymbolResults();
}

void BacktestEngine::buildExchangeResults() {
    for (const auto& [exchange, sim_exchange] : exchanges_) {
        BacktestResult::ExchangeResult ex_result;
        ex_result.exchange = exchange;
        ex_result.final_equity = sim_exchange->getTotalEquity(config_.default_quote_currency);

        // Calculate return
        auto balance_it = config_.initial_balances.find(
            std::string(core::exchange_to_string(exchange)));
        double initial = config_.default_initial_capital;
        if (balance_it != config_.initial_balances.end()) {
            auto quote_it = balance_it->second.find(config_.default_quote_currency);
            if (quote_it != balance_it->second.end()) {
                initial = quote_it->second;
            }
        }
        ex_result.total_return = ex_result.final_equity - initial;

        ex_result.trades = sim_exchange->totalOrdersFilled();
        ex_result.volume = sim_exchange->totalVolume();
        ex_result.fees = sim_exchange->totalFeessPaid();

        result_.exchange_results.push_back(ex_result);
    }
}

void BacktestEngine::buildSymbolResults() {
    // Aggregate by symbol from trades
    std::unordered_map<std::string, BacktestResult::SymbolResult> symbol_map;

    for (const auto& trade : result_.trades) {
        std::string key = std::string(trade.symbol.view()) + "_" +
                          std::string(core::exchange_to_string(trade.exchange));

        auto& sr = symbol_map[key];
        sr.symbol = trade.symbol;
        sr.exchange = trade.exchange;
        sr.trades++;
        sr.pnl += trade.realized_pnl;
        sr.volume += trade.notional();
    }

    // Calculate win rates
    for (auto& [key, sr] : symbol_map) {
        uint64_t wins = 0;
        for (const auto& trade : result_.trades) {
            std::string trade_key = std::string(trade.symbol.view()) + "_" +
                                    std::string(core::exchange_to_string(trade.exchange));
            if (trade_key == key && trade.realized_pnl > 0) {
                wins++;
            }
        }
        sr.win_rate = sr.trades > 0 ?
            (static_cast<double>(wins) / sr.trades) * 100.0 : 0.0;

        result_.symbol_results.push_back(sr);
    }

    // Sort by P/L descending
    std::sort(result_.symbol_results.begin(), result_.symbol_results.end(),
        [](const BacktestResult::SymbolResult& a, const BacktestResult::SymbolResult& b) {
            return a.pnl > b.pnl;
        });
}

// ============================================================================
// BacktestRunner Implementation
// ============================================================================

void BacktestRunner::setBaseConfig(const BacktestConfig& config) {
    base_config_ = config;
}

void BacktestRunner::setDataFeed(std::shared_ptr<DataFeed> feed) {
    feed_ = std::move(feed);
}

void BacktestRunner::setStrategyFactory(StrategyFactory factory) {
    strategy_factory_ = std::move(factory);
}

void BacktestRunner::addParameterSet(
    const std::string& name,
    const std::unordered_map<std::string, std::string>& params) {

    ParameterSet ps;
    ps.name = name;
    ps.params = params;
    parameter_sets_.push_back(ps);
}

void BacktestRunner::generateParameterGrid(
    const std::string& param_name,
    const std::vector<std::string>& values) {

    if (parameter_sets_.empty()) {
        // Create initial parameter sets
        for (const auto& value : values) {
            ParameterSet ps;
            ps.name = param_name + "=" + value;
            ps.params[param_name] = value;
            parameter_sets_.push_back(ps);
        }
    } else {
        // Expand existing sets
        std::vector<ParameterSet> new_sets;

        for (const auto& existing : parameter_sets_) {
            for (const auto& value : values) {
                ParameterSet ps = existing;
                ps.name = existing.name + "_" + param_name + "=" + value;
                ps.params[param_name] = value;
                new_sets.push_back(ps);
            }
        }

        parameter_sets_ = std::move(new_sets);
    }
}

void BacktestRunner::setProgressCallback(RunnerProgressCallback callback) {
    progress_callback_ = std::move(callback);
}

std::vector<BacktestResult> BacktestRunner::runAll() {
    std::vector<BacktestResult> results;

    for (size_t i = 0; i < parameter_sets_.size(); ++i) {
        const auto& ps = parameter_sets_[i];

        if (progress_callback_) {
            progress_callback_(i, parameter_sets_.size(), ps.name);
        }

        // Create engine
        BacktestEngine engine;

        // Merge parameters
        BacktestConfig config = base_config_;
        for (const auto& [key, value] : ps.params) {
            config.strategy_params[key] = value;
        }

        engine.setConfig(config);
        engine.setDataFeed(feed_);

        // Create fresh strategy instance
        auto strategy = strategy_factory_();
        engine.setStrategy(strategy);

        // Run
        auto result = engine.run();
        results.push_back(result);

        // Reset data feed for next run
        if (feed_) {
            feed_->reset();
        }
    }

    if (progress_callback_) {
        progress_callback_(parameter_sets_.size(), parameter_sets_.size(), "Complete");
    }

    return results;
}

std::vector<BacktestResult> BacktestRunner::runAllParallel(size_t max_threads) {
    if (max_threads == 0) {
        max_threads = std::thread::hardware_concurrency();
    }

    std::vector<BacktestResult> results(parameter_sets_.size());
    std::atomic<size_t> completed{0};
    std::mutex progress_mutex;

    // Create thread pool
    std::vector<std::thread> threads;
    std::atomic<size_t> next_task{0};

    auto worker = [&]() {
        while (true) {
            size_t task_idx = next_task.fetch_add(1);
            if (task_idx >= parameter_sets_.size()) break;

            const auto& ps = parameter_sets_[task_idx];

            // Create engine (each thread needs its own data feed copy)
            BacktestEngine engine;

            BacktestConfig config = base_config_;
            for (const auto& [key, value] : ps.params) {
                config.strategy_params[key] = value;
            }

            engine.setConfig(config);

            // Create a fresh data feed for this thread
            DataFeedConfig feed_config;
            feed_config.start_time = config.start_time;
            feed_config.end_time = config.end_time;
            // Note: In production, you'd need to clone the data feed properly
            engine.setDataFeed(feed_);

            auto strategy = strategy_factory_();
            engine.setStrategy(strategy);

            results[task_idx] = engine.run();

            size_t done = ++completed;
            if (progress_callback_) {
                std::lock_guard<std::mutex> lock(progress_mutex);
                progress_callback_(done, parameter_sets_.size(), ps.name);
            }
        }
    };

    // Start threads
    for (size_t i = 0; i < std::min(max_threads, parameter_sets_.size()); ++i) {
        threads.emplace_back(worker);
    }

    // Wait for completion
    for (auto& thread : threads) {
        thread.join();
    }

    return results;
}

// ============================================================================
// WalkForwardOptimizer Implementation
// ============================================================================

void WalkForwardOptimizer::setConfig(const WalkForwardConfig& config) {
    config_ = config;
}

void WalkForwardOptimizer::setDataFeed(std::shared_ptr<DataFeed> feed) {
    feed_ = std::move(feed);
}

void WalkForwardOptimizer::setStrategyFactory(
    std::function<std::shared_ptr<Strategy>()> factory) {
    strategy_factory_ = std::move(factory);
}

void WalkForwardOptimizer::setProgressCallback(WFProgressCallback callback) {
    progress_callback_ = std::move(callback);
}

std::vector<std::pair<core::Timestamp, core::Timestamp>>
WalkForwardOptimizer::generateWindows() const {

    std::vector<std::pair<core::Timestamp, core::Timestamp>> windows;

    uint64_t in_sample_us = config_.in_sample_days * 24ULL * 60 * 60 * 1000000;
    uint64_t out_sample_us = config_.out_of_sample_days * 24ULL * 60 * 60 * 1000000;
    uint64_t step_us = config_.step_days * 24ULL * 60 * 60 * 1000000;
    uint64_t total_window_us = in_sample_us + out_sample_us;

    core::Timestamp current = config_.total_start;

    while (current.nanos + total_window_us * 1000ULL <= config_.total_end.nanos) {
        windows.emplace_back(current,
            core::Timestamp{current.nanos + total_window_us * 1000ULL});
        current = core::Timestamp{current.nanos + step_us * 1000ULL};
    }

    return windows;
}

double WalkForwardOptimizer::extractMetric(
    const PerformanceMetrics& metrics,
    const std::string& metric_name) const {

    if (metric_name == "sharpe_ratio") return metrics.sharpe_ratio;
    if (metric_name == "sortino_ratio") return metrics.sortino_ratio;
    if (metric_name == "total_return") return metrics.total_return;
    if (metric_name == "total_return_pct") return metrics.total_return_pct;
    if (metric_name == "profit_factor") return metrics.profit_factor;
    if (metric_name == "win_rate") return metrics.win_rate;
    if (metric_name == "calmar_ratio") return metrics.calmar_ratio;
    if (metric_name == "max_drawdown") return -metrics.max_drawdown_pct;  // Negate for minimization

    return metrics.sharpe_ratio;  // Default
}

WalkForwardResult WalkForwardOptimizer::run() {
    WalkForwardResult result;

    auto windows = generateWindows();
    size_t total_windows = windows.size();

    if (total_windows == 0) {
        return result;
    }

    uint64_t in_sample_us = config_.in_sample_days * 24ULL * 60 * 60 * 1000000;

    // Process each window
    for (size_t w = 0; w < windows.size(); ++w) {
        if (progress_callback_) {
            progress_callback_(w, total_windows, "Processing window " + std::to_string(w + 1));
        }

        WalkForwardResult::Window window_result;
        window_result.in_sample_start = windows[w].first;
        window_result.in_sample_end = core::Timestamp{
            windows[w].first.nanos + in_sample_us * 1000ULL
        };
        window_result.out_of_sample_start = window_result.in_sample_end;
        window_result.out_of_sample_end = windows[w].second;

        // Generate all parameter combinations
        std::vector<std::unordered_map<std::string, std::string>> param_combinations;
        param_combinations.push_back({});  // Start with empty

        for (const auto& param : config_.parameters) {
            std::vector<std::unordered_map<std::string, std::string>> new_combinations;

            for (const auto& existing : param_combinations) {
                for (const auto& value : param.values) {
                    auto combo = existing;
                    combo[param.name] = value;
                    new_combinations.push_back(combo);
                }
            }

            param_combinations = std::move(new_combinations);
        }

        // Find best parameters in-sample
        double best_metric = config_.maximize ?
            -std::numeric_limits<double>::infinity() :
            std::numeric_limits<double>::infinity();
        std::unordered_map<std::string, std::string> best_params;
        PerformanceMetrics best_is_metrics;

        for (const auto& params : param_combinations) {
            BacktestEngine engine;

            BacktestConfig config;
            config.start_time = window_result.in_sample_start;
            config.end_time = window_result.in_sample_end;
            config.default_initial_capital = 100000.0;
            config.strategy_params = params;

            engine.setConfig(config);
            engine.setDataFeed(feed_);
            engine.setStrategy(strategy_factory_());

            auto bt_result = engine.run();
            double metric = extractMetric(bt_result.metrics, config_.optimization_metric);

            bool is_better = config_.maximize ?
                (metric > best_metric) : (metric < best_metric);

            if (is_better) {
                best_metric = metric;
                best_params = params;
                best_is_metrics = bt_result.metrics;
            }

            feed_->reset();
        }

        window_result.best_params = best_params;
        window_result.in_sample_metric = best_metric;
        window_result.in_sample_metrics = best_is_metrics;

        // Run out-of-sample with best parameters
        {
            BacktestEngine engine;

            BacktestConfig config;
            config.start_time = window_result.out_of_sample_start;
            config.end_time = window_result.out_of_sample_end;
            config.default_initial_capital = 100000.0;
            config.strategy_params = best_params;

            engine.setConfig(config);
            engine.setDataFeed(feed_);
            engine.setStrategy(strategy_factory_());

            auto bt_result = engine.run();
            window_result.out_of_sample_metric = extractMetric(
                bt_result.metrics, config_.optimization_metric);
            window_result.out_of_sample_metrics = bt_result.metrics;

            feed_->reset();
        }

        result.windows.push_back(window_result);
    }

    // Calculate aggregated metrics
    double sum_oos = 0.0;
    size_t positive_count = 0;

    for (const auto& window : result.windows) {
        sum_oos += window.out_of_sample_metric;
        if (window.out_of_sample_metric > 0) {
            positive_count++;
        }
    }

    result.average_out_of_sample_metric = sum_oos / result.windows.size();
    result.consistency_ratio = static_cast<double>(positive_count) / result.windows.size();

    // Calculate parameter stability
    if (!config_.parameters.empty() && result.windows.size() > 1) {
        size_t stable_params = 0;
        size_t total_params = config_.parameters.size();

        for (const auto& param : config_.parameters) {
            std::string first_value = result.windows[0].best_params[param.name];
            bool all_same = true;

            for (size_t i = 1; i < result.windows.size(); ++i) {
                if (result.windows[i].best_params[param.name] != first_value) {
                    all_same = false;
                    break;
                }
            }

            if (all_same) stable_params++;
        }

        result.parameter_stability = static_cast<double>(stable_params) / total_params;
    }

    if (progress_callback_) {
        progress_callback_(total_windows, total_windows, "Complete");
    }

    return result;
}

}  // namespace hft::backtesting
