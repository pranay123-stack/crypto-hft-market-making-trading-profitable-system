#include "trading/trading_engine.hpp"

#include <algorithm>
#include <numeric>
#include <sstream>
#include <iostream>
#include <cstring>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace hft::trading {

// ============================================================================
// Trading Engine Implementation
// ============================================================================

TradingEngine::TradingEngine() = default;

TradingEngine::~TradingEngine() {
    stop();
}

// ============================================================================
// Lifecycle Management
// ============================================================================

bool TradingEngine::initialize(const TradingEngineConfig& config) {
    if (initialized_) {
        return true;
    }

    config_ = config;

    // Create trading mode based on configuration
    switch (config.mode) {
        case core::TradingMode::Paper:
            paper_mode_ = create_paper_trading_mode();
            if (!paper_mode_->initialize(config.mode_config)) {
                return false;
            }
            trading_mode_ = paper_mode_;
            break;

        case core::TradingMode::Live:
            live_mode_ = create_live_trading_mode();
            if (!live_mode_->initialize(config.mode_config)) {
                return false;
            }
            trading_mode_ = live_mode_;
            break;

        case core::TradingMode::Backtest:
            // Backtest mode uses paper trading internally
            paper_mode_ = create_paper_trading_mode();
            if (!paper_mode_->initialize(config.mode_config)) {
                return false;
            }
            trading_mode_ = paper_mode_;
            break;
    }

    // Set up trading mode callbacks
    setup_mode_callbacks();

    initialized_ = true;
    return true;
}

bool TradingEngine::start() {
    if (!initialized_) {
        std::cerr << "Trading engine not initialized" << std::endl;
        return false;
    }

    if (running_.exchange(true)) {
        return true;  // Already running
    }

    shutdown_requested_ = false;
    start_time_ = std::chrono::steady_clock::now();
    events_processed_ = 0;

    // Start trading mode
    if (!trading_mode_->start()) {
        running_ = false;
        return false;
    }

    // Start event processing threads
    event_thread_ = std::make_unique<std::thread>([this]() {
        if (config_.thread_pinning.enabled &&
            config_.thread_pinning.event_loop_core >= 0) {
            set_thread_affinity(config_.thread_pinning.event_loop_core);
        }
        event_loop();
    });

    market_data_thread_ = std::make_unique<std::thread>([this]() {
        if (config_.thread_pinning.enabled &&
            config_.thread_pinning.market_data_core >= 0) {
            set_thread_affinity(config_.thread_pinning.market_data_core);
        }
        market_data_loop();
    });

    order_thread_ = std::make_unique<std::thread>([this]() {
        if (config_.thread_pinning.enabled &&
            config_.thread_pinning.order_routing_core >= 0) {
            set_thread_affinity(config_.thread_pinning.order_routing_core);
        }
        order_routing_loop();
    });

    timer_thread_ = std::make_unique<std::thread>([this]() {
        timer_loop();
    });

    // Start all enabled strategies
    {
        std::shared_lock lock(strategies_mutex_);
        for (auto& [id, entry] : strategies_) {
            if (entry.config.enabled) {
                if (entry.strategy->start()) {
                    entry.state = StrategyState::Running;
                } else {
                    entry.state = StrategyState::Error;
                    std::cerr << "Failed to start strategy: " << id << std::endl;
                }
            }
        }
    }

    return true;
}

void TradingEngine::stop() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    shutdown_requested_ = true;

    // Cancel all orders if configured
    if (config_.cancel_on_shutdown && trading_mode_) {
        cancel_all_orders();
    }

    // Stop all strategies
    {
        std::unique_lock lock(strategies_mutex_);
        for (auto& [id, entry] : strategies_) {
            if (entry.state == StrategyState::Running) {
                entry.strategy->stop();
                entry.state = StrategyState::Stopped;
            }
        }
    }

    // Stop trading mode
    if (trading_mode_) {
        trading_mode_->stop();
    }

    // Wake up all threads
    event_queue_.cv.notify_all();
    market_data_queue_.cv.notify_all();
    order_queue_.cv.notify_all();

    // Signal shutdown
    {
        std::lock_guard lock(shutdown_mutex_);
        shutdown_cv_.notify_all();
    }

    // Wait for threads with timeout
    auto join_with_timeout = [this](std::unique_ptr<std::thread>& thread) {
        if (thread && thread->joinable()) {
            thread->join();
        }
        thread.reset();
    };

    join_with_timeout(timer_thread_);
    join_with_timeout(order_thread_);
    join_with_timeout(market_data_thread_);
    join_with_timeout(event_thread_);
}

void TradingEngine::wait() {
    std::unique_lock lock(shutdown_mutex_);
    shutdown_cv_.wait(lock, [this]() {
        return !running_.load() || shutdown_requested_.load();
    });
}

bool TradingEngine::is_running() const {
    return running_.load();
}

void TradingEngine::request_shutdown() {
    shutdown_requested_ = true;

    // Push shutdown event
    EngineEvent event;
    event.type = EventType::Shutdown;
    event.timestamp = current_time();
    push_event(std::move(event));
}

// ============================================================================
// Strategy Management
// ============================================================================

bool TradingEngine::register_strategy(StrategyPtr strategy,
                                       const StrategyConfig& config) {
    if (!strategy) {
        return false;
    }

    std::unique_lock lock(strategies_mutex_);

    // Check for duplicate ID
    if (strategies_.find(config.id) != strategies_.end()) {
        std::cerr << "Strategy with ID " << config.id << " already registered"
                  << std::endl;
        return false;
    }

    // Initialize strategy
    if (!strategy->initialize(config, this)) {
        std::cerr << "Failed to initialize strategy: " << config.id << std::endl;
        return false;
    }

    // Register
    StrategyEntry entry;
    entry.strategy = strategy;
    entry.config = config;
    entry.state = StrategyState::Stopped;
    strategies_[config.id] = entry;

    // Set up subscriptions
    {
        std::lock_guard sub_lock(subscriptions_mutex_);
        for (const auto& symbol : config.symbols) {
            for (const auto& exchange : config.exchanges) {
                SubscriptionKey key{exchange, symbol};
                subscriptions_[key].push_back(config.id);
            }
        }
    }

    return true;
}

bool TradingEngine::unregister_strategy(const std::string& strategy_id) {
    std::unique_lock lock(strategies_mutex_);

    auto it = strategies_.find(strategy_id);
    if (it == strategies_.end()) {
        return false;
    }

    // Stop if running
    if (it->second.state == StrategyState::Running) {
        it->second.strategy->stop();
    }

    // Remove subscriptions
    {
        std::lock_guard sub_lock(subscriptions_mutex_);
        for (auto& [key, ids] : subscriptions_) {
            ids.erase(std::remove(ids.begin(), ids.end(), strategy_id), ids.end());
        }
    }

    strategies_.erase(it);
    return true;
}

StrategyPtr TradingEngine::get_strategy(const std::string& strategy_id) const {
    std::shared_lock lock(strategies_mutex_);
    auto it = strategies_.find(strategy_id);
    if (it != strategies_.end()) {
        return it->second.strategy;
    }
    return nullptr;
}

std::vector<StrategyPtr> TradingEngine::get_all_strategies() const {
    std::shared_lock lock(strategies_mutex_);
    std::vector<StrategyPtr> result;
    result.reserve(strategies_.size());
    for (const auto& [id, entry] : strategies_) {
        result.push_back(entry.strategy);
    }
    return result;
}

bool TradingEngine::start_strategy(const std::string& strategy_id) {
    std::unique_lock lock(strategies_mutex_);
    auto it = strategies_.find(strategy_id);
    if (it == strategies_.end()) {
        return false;
    }

    if (it->second.state == StrategyState::Running) {
        return true;  // Already running
    }

    if (it->second.strategy->start()) {
        it->second.state = StrategyState::Running;
        return true;
    }

    it->second.state = StrategyState::Error;
    return false;
}

bool TradingEngine::stop_strategy(const std::string& strategy_id) {
    std::unique_lock lock(strategies_mutex_);
    auto it = strategies_.find(strategy_id);
    if (it == strategies_.end()) {
        return false;
    }

    if (it->second.state != StrategyState::Running) {
        return true;  // Already stopped
    }

    it->second.strategy->stop();
    it->second.state = StrategyState::Stopped;
    return true;
}

// ============================================================================
// Trading Mode Management
// ============================================================================

TradingModePtr TradingEngine::get_trading_mode() const {
    return trading_mode_;
}

bool TradingEngine::switch_mode(core::TradingMode mode,
                                 const TradingModeConfig& config) {
    if (running_) {
        std::cerr << "Cannot switch mode while engine is running" << std::endl;
        return false;
    }

    // Create new trading mode
    TradingModePtr new_mode;

    switch (mode) {
        case core::TradingMode::Paper:
            paper_mode_ = create_paper_trading_mode();
            if (!paper_mode_->initialize(config)) {
                return false;
            }
            new_mode = paper_mode_;
            break;

        case core::TradingMode::Live:
            live_mode_ = create_live_trading_mode();
            if (!live_mode_->initialize(config)) {
                return false;
            }
            new_mode = live_mode_;
            break;

        case core::TradingMode::Backtest:
            paper_mode_ = create_paper_trading_mode();
            if (!paper_mode_->initialize(config)) {
                return false;
            }
            new_mode = paper_mode_;
            break;
    }

    trading_mode_ = new_mode;
    config_.mode = mode;
    config_.mode_config = config;

    setup_mode_callbacks();

    return true;
}

bool TradingEngine::is_paper_trading() const {
    return trading_mode_ && trading_mode_->mode() == core::TradingMode::Paper;
}

bool TradingEngine::is_live_trading() const {
    return trading_mode_ && trading_mode_->mode() == core::TradingMode::Live;
}

// ============================================================================
// Market Data Interface
// ============================================================================

void TradingEngine::on_order_book(const OrderBook& book) {
    MarketDataUpdate update;
    update.type = MarketDataUpdate::Type::OrderBook;
    update.exchange = book.exchange;
    update.symbol = book.symbol;
    update.order_book = book;
    update.best_bid = book.best_bid();
    update.best_ask = book.best_ask();
    update.last_price = book.mid_price();
    update.timestamp = book.timestamp;

    on_market_data(update);
}

void TradingEngine::on_trade(const TradeTick& trade) {
    MarketDataUpdate update;
    update.type = MarketDataUpdate::Type::Trade;
    update.exchange = trade.exchange;
    update.symbol = trade.symbol;
    update.trade = trade;
    update.last_price = trade.price;
    update.timestamp = trade.timestamp;

    on_market_data(update);
}

void TradingEngine::on_market_data(const MarketDataUpdate& update) {
    // Update trading mode with market price
    if (trading_mode_) {
        trading_mode_->update_market_price(
            update.exchange,
            update.symbol,
            update.best_bid,
            update.best_ask,
            update.last_price,
            update.timestamp);
    }

    // Queue for distribution
    EngineEvent event;
    event.type = EventType::MarketData;
    event.timestamp = update.timestamp;
    event.market_data = update;

    {
        std::lock_guard lock(market_data_queue_.mutex);
        if (market_data_queue_.size < config_.market_data_queue_size) {
            market_data_queue_.queue.push(std::move(event));
            market_data_queue_.size++;
            market_data_queue_.cv.notify_one();
        }
        // Drop if queue is full (prevent backpressure)
    }
}

void TradingEngine::subscribe(core::Exchange exchange, const core::Symbol& symbol) {
    // Subscription tracking is handled internally
    // Actual subscription to exchange data feeds would be done by market data handler
    std::lock_guard lock(subscriptions_mutex_);
    SubscriptionKey key{exchange, symbol};
    // Mark as subscribed (empty list means engine-level subscription)
    if (subscriptions_.find(key) == subscriptions_.end()) {
        subscriptions_[key] = std::vector<std::string>{};
    }
}

void TradingEngine::unsubscribe(core::Exchange exchange, const core::Symbol& symbol) {
    std::lock_guard lock(subscriptions_mutex_);
    SubscriptionKey key{exchange, symbol};
    subscriptions_.erase(key);
}

// ============================================================================
// Order Interface
// ============================================================================

OrderResponse TradingEngine::submit_order(const OrderRequest& request) {
    if (!trading_mode_) {
        OrderResponse response;
        response.status = core::OrderStatus::Rejected;
        response.error_code = core::ErrorCode::InternalError;
        response.error_message = "No trading mode configured";
        return response;
    }

    // Track order to strategy mapping
    {
        std::lock_guard lock(order_tracking_mutex_);
        order_to_strategy_[request.client_order_id.value] = request.strategy_id;
    }

    // Submit to trading mode
    auto response = trading_mode_->submit_order(request);

    // Log if configured
    if (config_.log_orders) {
        std::cout << "[ORDER] " << core::side_to_string(request.side) << " "
                  << request.quantity.to_double() << " "
                  << request.symbol.view() << " @ "
                  << request.price.to_double()
                  << " -> " << core::order_status_to_string(response.status)
                  << std::endl;
    }

    return response;
}

CancelResponse TradingEngine::cancel_order(const CancelRequest& request) {
    if (!trading_mode_) {
        CancelResponse response;
        response.success = false;
        response.error_code = core::ErrorCode::InternalError;
        response.error_message = "No trading mode configured";
        return response;
    }

    return trading_mode_->cancel_order(request);
}

uint32_t TradingEngine::cancel_all_orders(core::Exchange exchange,
                                           const core::Symbol& symbol) {
    if (!trading_mode_) {
        return 0;
    }

    return trading_mode_->cancel_all_orders(exchange, symbol);
}

std::optional<Position> TradingEngine::get_position(
    core::Exchange exchange, const core::Symbol& symbol) const {
    if (!trading_mode_) {
        return std::nullopt;
    }
    return trading_mode_->get_position(exchange, symbol);
}

std::vector<Position> TradingEngine::get_all_positions(
    core::Exchange exchange) const {
    if (!trading_mode_) {
        return {};
    }
    return trading_mode_->get_all_positions(exchange);
}

std::optional<Balance> TradingEngine::get_balance(
    core::Exchange exchange, const std::string& asset) const {
    if (!trading_mode_) {
        return std::nullopt;
    }
    return trading_mode_->get_balance(exchange, asset);
}

std::vector<Balance> TradingEngine::get_all_balances(
    core::Exchange exchange) const {
    if (!trading_mode_) {
        return {};
    }
    return trading_mode_->get_all_balances(exchange);
}

// ============================================================================
// Event Queue Interface
// ============================================================================

bool TradingEngine::push_event(EngineEvent event) {
    std::lock_guard lock(event_queue_.mutex);
    if (event_queue_.size >= config_.event_queue_size) {
        return false;
    }
    event_queue_.queue.push(std::move(event));
    event_queue_.size++;
    event_queue_.cv.notify_one();
    return true;
}

core::Timestamp TradingEngine::current_time() const {
    return current_timestamp();
}

// ============================================================================
// Statistics and Diagnostics
// ============================================================================

const TradingEngineConfig& TradingEngine::config() const {
    return config_;
}

TradingEngine::LatencyStats TradingEngine::get_latency_stats() const {
    LatencyStats stats;

    std::lock_guard lock(latency_stats_.mutex);
    stats.count = latency_stats_.count.load();

    if (stats.count == 0) {
        return stats;
    }

    uint64_t total = latency_stats_.total_ns.load();
    stats.mean_us = (total / stats.count) / 1000.0;
    stats.max_us = latency_stats_.max_ns.load() / 1000.0;

    if (!latency_stats_.samples.empty()) {
        auto samples = latency_stats_.samples;
        std::sort(samples.begin(), samples.end());
        size_t n = samples.size();
        stats.median_us = samples[n / 2] / 1000.0;
        stats.p99_us = samples[static_cast<size_t>(n * 0.99)] / 1000.0;
    }

    return stats;
}

double TradingEngine::events_per_second() const {
    auto elapsed = std::chrono::steady_clock::now() - start_time_;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    if (seconds == 0) {
        return 0.0;
    }
    return static_cast<double>(events_processed_.load()) / seconds;
}

TradingEngine::QueueDepths TradingEngine::get_queue_depths() const {
    QueueDepths depths;
    depths.event_queue = event_queue_.size.load();
    depths.market_data_queue = market_data_queue_.size.load();
    depths.order_queue = order_queue_.size.load();
    return depths;
}

void TradingEngine::reset_statistics() {
    latency_stats_.count = 0;
    latency_stats_.total_ns = 0;
    latency_stats_.max_ns = 0;
    {
        std::lock_guard lock(latency_stats_.mutex);
        latency_stats_.samples.clear();
    }
    events_processed_ = 0;
    start_time_ = std::chrono::steady_clock::now();
}

// ============================================================================
// Internal Methods
// ============================================================================

void TradingEngine::event_loop() {
    while (running_.load() && !shutdown_requested_.load()) {
        EngineEvent event;
        bool got_event = false;

        {
            std::unique_lock lock(event_queue_.mutex);

            if (config_.busy_poll) {
                // Busy polling - check without waiting
                if (!event_queue_.queue.empty()) {
                    event = std::move(event_queue_.queue.front());
                    event_queue_.queue.pop();
                    event_queue_.size--;
                    got_event = true;
                }
            } else {
                // Wait with timeout
                if (event_queue_.cv.wait_for(
                        lock,
                        std::chrono::microseconds(config_.poll_timeout_us),
                        [this]() {
                            return !event_queue_.queue.empty() ||
                                   shutdown_requested_.load();
                        })) {
                    if (!event_queue_.queue.empty()) {
                        event = std::move(event_queue_.queue.front());
                        event_queue_.queue.pop();
                        event_queue_.size--;
                        got_event = true;
                    }
                }
            }
        }

        if (got_event) {
            auto start = std::chrono::high_resolution_clock::now();
            process_event(event);
            auto end = std::chrono::high_resolution_clock::now();

            auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count();
            update_latency_stats(latency);
            events_processed_++;
        }
    }
}

void TradingEngine::market_data_loop() {
    while (running_.load() && !shutdown_requested_.load()) {
        EngineEvent event;
        bool got_event = false;

        {
            std::unique_lock lock(market_data_queue_.mutex);

            if (config_.busy_poll) {
                if (!market_data_queue_.queue.empty()) {
                    event = std::move(market_data_queue_.queue.front());
                    market_data_queue_.queue.pop();
                    market_data_queue_.size--;
                    got_event = true;
                }
            } else {
                if (market_data_queue_.cv.wait_for(
                        lock,
                        std::chrono::microseconds(config_.poll_timeout_us),
                        [this]() {
                            return !market_data_queue_.queue.empty() ||
                                   shutdown_requested_.load();
                        })) {
                    if (!market_data_queue_.queue.empty()) {
                        event = std::move(market_data_queue_.queue.front());
                        market_data_queue_.queue.pop();
                        market_data_queue_.size--;
                        got_event = true;
                    }
                }
            }
        }

        if (got_event) {
            distribute_market_data(event.market_data);
        }
    }
}

void TradingEngine::order_routing_loop() {
    while (running_.load() && !shutdown_requested_.load()) {
        EngineEvent event;
        bool got_event = false;

        {
            std::unique_lock lock(order_queue_.mutex);

            if (order_queue_.cv.wait_for(
                    lock,
                    std::chrono::milliseconds(10),
                    [this]() {
                        return !order_queue_.queue.empty() ||
                               shutdown_requested_.load();
                    })) {
                if (!order_queue_.queue.empty()) {
                    event = std::move(order_queue_.queue.front());
                    order_queue_.queue.pop();
                    order_queue_.size--;
                    got_event = true;
                }
            }
        }

        if (got_event) {
            switch (event.type) {
                case EventType::OrderResponse:
                    distribute_order_response(event.order_response);
                    break;
                case EventType::Execution:
                    distribute_execution(event.execution);
                    break;
                case EventType::PositionUpdate:
                    distribute_position_update(event.position);
                    break;
                default:
                    break;
            }
        }
    }
}

void TradingEngine::timer_loop() {
    while (running_.load() && !shutdown_requested_.load()) {
        fire_timers();

        // Sleep for timer interval
        std::this_thread::sleep_for(
            std::chrono::microseconds(config_.timer_interval_us));
    }
}

void TradingEngine::process_event(const EngineEvent& event) {
    switch (event.type) {
        case EventType::MarketData:
            distribute_market_data(event.market_data);
            break;

        case EventType::OrderResponse:
            distribute_order_response(event.order_response);
            break;

        case EventType::Execution:
            distribute_execution(event.execution);
            break;

        case EventType::PositionUpdate:
            distribute_position_update(event.position);
            break;

        case EventType::Timer:
            fire_timers();
            break;

        case EventType::Shutdown:
            shutdown_requested_ = true;
            break;

        case EventType::BalanceUpdate:
        case EventType::Custom:
            // Handle as needed
            break;
    }
}

void TradingEngine::distribute_market_data(const MarketDataUpdate& update) {
    std::vector<std::string> strategy_ids;

    // Find subscribed strategies
    {
        std::lock_guard lock(subscriptions_mutex_);
        SubscriptionKey key{update.exchange, update.symbol};
        auto it = subscriptions_.find(key);
        if (it != subscriptions_.end()) {
            strategy_ids = it->second;
        }
    }

    // Distribute to each subscribed strategy
    {
        std::shared_lock lock(strategies_mutex_);
        for (const auto& id : strategy_ids) {
            auto it = strategies_.find(id);
            if (it != strategies_.end() &&
                it->second.state == StrategyState::Running) {
                try {
                    it->second.strategy->on_market_data(update);

                    if (update.type == MarketDataUpdate::Type::OrderBook) {
                        it->second.strategy->on_order_book(update.order_book);
                    } else if (update.type == MarketDataUpdate::Type::Trade) {
                        it->second.strategy->on_trade(update.trade);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Strategy " << id << " error: " << e.what()
                              << std::endl;
                }
            }
        }
    }
}

void TradingEngine::distribute_order_response(const OrderResponse& response) {
    std::string strategy_id;

    // Find originating strategy
    {
        std::lock_guard lock(order_tracking_mutex_);
        auto it = order_to_strategy_.find(response.client_order_id.value);
        if (it != order_to_strategy_.end()) {
            strategy_id = it->second;
            if (core::is_terminal_status(response.status)) {
                order_to_strategy_.erase(it);
            }
        }
    }

    if (!strategy_id.empty()) {
        std::shared_lock lock(strategies_mutex_);
        auto it = strategies_.find(strategy_id);
        if (it != strategies_.end() &&
            it->second.state == StrategyState::Running) {
            try {
                it->second.strategy->on_order_response(response);
            } catch (const std::exception& e) {
                std::cerr << "Strategy " << strategy_id << " error: " << e.what()
                          << std::endl;
            }
        }
    }
}

void TradingEngine::distribute_execution(const ExecutionReport& execution) {
    std::string strategy_id;

    // Find originating strategy
    {
        std::lock_guard lock(order_tracking_mutex_);
        auto it = order_to_strategy_.find(execution.client_order_id.value);
        if (it != order_to_strategy_.end()) {
            strategy_id = it->second;
        }
    }

    if (config_.log_executions) {
        std::cout << "[EXEC] " << core::side_to_string(execution.side) << " "
                  << execution.quantity.to_double() << " "
                  << execution.symbol.view() << " @ "
                  << execution.price.to_double() << std::endl;
    }

    if (!strategy_id.empty()) {
        std::shared_lock lock(strategies_mutex_);
        auto it = strategies_.find(strategy_id);
        if (it != strategies_.end() &&
            it->second.state == StrategyState::Running) {
            try {
                it->second.strategy->on_execution(execution);
            } catch (const std::exception& e) {
                std::cerr << "Strategy " << strategy_id << " error: " << e.what()
                          << std::endl;
            }
        }
    }
}

void TradingEngine::distribute_position_update(const Position& position) {
    // Notify all running strategies
    std::shared_lock lock(strategies_mutex_);
    for (const auto& [id, entry] : strategies_) {
        if (entry.state == StrategyState::Running) {
            // Check if strategy is subscribed to this symbol
            bool subscribed = false;
            for (const auto& sym : entry.config.symbols) {
                if (sym == position.symbol) {
                    subscribed = true;
                    break;
                }
            }

            if (subscribed) {
                try {
                    entry.strategy->on_position_update(position);
                } catch (const std::exception& e) {
                    std::cerr << "Strategy " << id << " error: " << e.what()
                              << std::endl;
                }
            }
        }
    }
}

void TradingEngine::fire_timers() {
    auto now = current_time();

    std::shared_lock lock(strategies_mutex_);
    for (const auto& [id, entry] : strategies_) {
        if (entry.state == StrategyState::Running) {
            try {
                entry.strategy->on_timer(now);
            } catch (const std::exception& e) {
                std::cerr << "Strategy " << id << " timer error: " << e.what()
                          << std::endl;
            }
        }
    }
}

void TradingEngine::set_thread_affinity(int core) {
#ifdef __linux__
    if (core >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#else
    // Thread pinning not supported on this platform
    (void)core;
#endif
}

void TradingEngine::update_latency_stats(uint64_t latency_ns) {
    latency_stats_.count++;
    latency_stats_.total_ns += latency_ns;

    uint64_t current_max = latency_stats_.max_ns.load();
    while (latency_ns > current_max) {
        if (latency_stats_.max_ns.compare_exchange_weak(current_max, latency_ns)) {
            break;
        }
    }

    // Sample for percentile calculation (keep last 10000)
    {
        std::lock_guard lock(latency_stats_.mutex);
        if (latency_stats_.samples.size() >= 10000) {
            latency_stats_.samples.erase(latency_stats_.samples.begin());
        }
        latency_stats_.samples.push_back(latency_ns);
    }
}

void TradingEngine::setup_mode_callbacks() {
    if (!trading_mode_) {
        return;
    }

    trading_mode_->set_on_order_response([this](const OrderResponse& response) {
        EngineEvent event;
        event.type = EventType::OrderResponse;
        event.timestamp = response.timestamp;
        event.order_response = response;

        std::lock_guard lock(order_queue_.mutex);
        if (order_queue_.size < config_.order_queue_size) {
            order_queue_.queue.push(std::move(event));
            order_queue_.size++;
            order_queue_.cv.notify_one();
        }
    });

    trading_mode_->set_on_execution([this](const ExecutionReport& execution) {
        EngineEvent event;
        event.type = EventType::Execution;
        event.timestamp = execution.timestamp;
        event.execution = execution;

        std::lock_guard lock(order_queue_.mutex);
        if (order_queue_.size < config_.order_queue_size) {
            order_queue_.queue.push(std::move(event));
            order_queue_.size++;
            order_queue_.cv.notify_one();
        }
    });

    trading_mode_->set_on_position_update([this](const Position& position) {
        EngineEvent event;
        event.type = EventType::PositionUpdate;
        event.timestamp = position.updated_at;
        event.position = position;

        std::lock_guard lock(order_queue_.mutex);
        if (order_queue_.size < config_.order_queue_size) {
            order_queue_.queue.push(std::move(event));
            order_queue_.size++;
            order_queue_.cv.notify_one();
        }
    });

    trading_mode_->set_on_error([](core::ErrorCode code, const std::string& message) {
        std::cerr << "[ERROR] " << core::error_code_to_string(code)
                  << ": " << message << std::endl;
    });
}

// ============================================================================
// Factory Functions
// ============================================================================

TradingEnginePtr create_trading_engine() {
    return std::make_shared<TradingEngine>();
}

TradingEngineConfig create_default_engine_config() {
    TradingEngineConfig config;

    config.name = "HFT_Engine";
    config.mode = core::TradingMode::Paper;

    // Mode config
    config.mode_config.mode = core::TradingMode::Paper;
    config.mode_config.initial_balance = core::Price::from_double(100000.0);
    config.mode_config.base_currency = "USDT";

    // Threading
    config.event_queue_size = 100000;
    config.market_data_queue_size = 100000;
    config.order_queue_size = 10000;
    config.timer_interval_us = 1000;

    // Thread pinning disabled by default
    config.thread_pinning.enabled = false;

    // Event loop
    config.busy_poll = false;
    config.poll_timeout_us = 100;

    // Shutdown
    config.shutdown_timeout_ms = 5000;
    config.cancel_on_shutdown = true;

    // Logging
    config.log_events = false;
    config.log_orders = true;
    config.log_executions = true;

    return config;
}

// ============================================================================
// Base Strategy Implementation
// ============================================================================

BaseStrategy::BaseStrategy() = default;

BaseStrategy::~BaseStrategy() {
    stop();
}

bool BaseStrategy::initialize(const StrategyConfig& config,
                               TradingEngine* engine) {
    config_ = config;
    engine_ = engine;
    return true;
}

bool BaseStrategy::start() {
    StrategyState expected = StrategyState::Stopped;
    if (state_.compare_exchange_strong(expected, StrategyState::Starting)) {
        state_ = StrategyState::Running;
        return true;
    }
    return state_.load() == StrategyState::Running;
}

void BaseStrategy::stop() {
    StrategyState current = state_.load();
    if (current == StrategyState::Running || current == StrategyState::Starting) {
        state_ = StrategyState::Stopping;
        state_ = StrategyState::Stopped;
    }
}

StrategyState BaseStrategy::state() const {
    return state_.load();
}

const std::string& BaseStrategy::id() const {
    return config_.id;
}

const std::string& BaseStrategy::name() const {
    return config_.name;
}

void BaseStrategy::on_order_book(const OrderBook& /*book*/) {
    // Default: do nothing
}

void BaseStrategy::on_trade(const TradeTick& /*trade*/) {
    // Default: do nothing
}

void BaseStrategy::on_market_data(const MarketDataUpdate& /*update*/) {
    // Default: do nothing
}

void BaseStrategy::on_order_response(const OrderResponse& /*response*/) {
    // Default: do nothing
}

void BaseStrategy::on_execution(const ExecutionReport& /*execution*/) {
    // Default: do nothing
}

void BaseStrategy::on_position_update(const Position& /*position*/) {
    // Default: do nothing
}

void BaseStrategy::on_timer(core::Timestamp /*timestamp*/) {
    // Default: do nothing
}

OrderResponse BaseStrategy::send_order(const OrderRequest& request) {
    if (!engine_) {
        OrderResponse response;
        response.status = core::OrderStatus::Rejected;
        response.error_code = core::ErrorCode::InternalError;
        response.error_message = "No engine reference";
        return response;
    }

    OrderRequest req = request;
    req.strategy_id = config_.id;
    return engine_->submit_order(req);
}

CancelResponse BaseStrategy::send_cancel(const CancelRequest& request) {
    if (!engine_) {
        CancelResponse response;
        response.success = false;
        response.error_code = core::ErrorCode::InternalError;
        response.error_message = "No engine reference";
        return response;
    }

    return engine_->cancel_order(request);
}

std::optional<Position> BaseStrategy::position(core::Exchange exchange,
                                                const core::Symbol& symbol) const {
    if (!engine_) {
        return std::nullopt;
    }
    return engine_->get_position(exchange, symbol);
}

std::optional<Balance> BaseStrategy::balance(core::Exchange exchange,
                                              const std::string& asset) const {
    if (!engine_) {
        return std::nullopt;
    }
    return engine_->get_balance(exchange, asset);
}

void BaseStrategy::log(const std::string& message) {
    std::cout << "[" << config_.id << "] " << message << std::endl;
}

}  // namespace hft::trading
