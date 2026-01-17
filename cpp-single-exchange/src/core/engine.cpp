#include "core/engine.hpp"
#include "exchange/binance_client.hpp"
#include <chrono>

namespace hft {

// ============================================================================
// TradingEngine Implementation
// ============================================================================

TradingEngine::TradingEngine(const Config& config)
    : config_(config)
{
    // Initialize logger
    logger_ = std::make_unique<Logger>("engine", LogLevel::INFO);

    // Initialize orderbook
    Symbol sym(config.trading.symbol);
    orderbook_ = std::make_unique<OrderBook>(sym);

    // Initialize strategy
    strategy_ = std::make_unique<MarketMaker>(config.strategy);

    // Initialize risk manager
    risk_manager_ = std::make_unique<RiskManager>(config.risk);

    // Initialize exchange client
    if (config.exchange.name == "binance") {
        BinanceConfig binance_config;
        binance_config.api_key = config.exchange.api_key;
        binance_config.api_secret = config.exchange.api_secret;
        if (config.trading.paper_trading) {
            binance_config.set_testnet();
        }
        exchange_client_ = std::make_unique<BinanceClient>(binance_config);
    }

    // Setup strategy callbacks
    strategy_->set_order_callback([this](const Order& order) -> OrderId {
        return send_order(order);
    });

    strategy_->set_cancel_callback([this](OrderId id) -> bool {
        return cancel_order(id);
    });

    // Setup risk manager kill switch callback
    risk_manager_->set_kill_switch_callback([this](const std::string& reason) {
        logger_->error("KILL SWITCH ACTIVATED: %s", reason.c_str());
        trading_enabled_ = false;
        // Cancel all open orders
        if (exchange_client_) {
            Symbol sym(config_.trading.symbol);
            exchange_client_->cancel_all_orders(sym);
        }
    });

    logger_->info("Trading engine initialized");
}

TradingEngine::~TradingEngine() {
    if (running_) {
        stop();
    }
}

void TradingEngine::start() {
    if (running_) {
        logger_->warn("Engine already running");
        return;
    }

    running_ = true;
    logger_->info("Starting trading engine...");

    // Connect to exchange
    if (exchange_client_) {
        ExchangeCallbacks callbacks;

        callbacks.on_tick = [this](const Tick& tick) {
            on_market_data(tick);
        };

        callbacks.on_order_update = [this](const Order& order) {
            on_order_update(order);
        };

        callbacks.on_trade = [this](const Trade& trade) {
            on_trade(trade);
        };

        callbacks.on_error = [this](const std::string& error) {
            logger_->error("Exchange error: %s", error.c_str());
        };

        callbacks.on_connected = [this]() {
            logger_->info("Connected to exchange");
            trading_enabled_ = true;
        };

        callbacks.on_disconnected = [this]() {
            logger_->warn("Disconnected from exchange");
            trading_enabled_ = false;
        };

        exchange_client_->set_callbacks(callbacks);

        if (!exchange_client_->connect()) {
            logger_->error("Failed to connect to exchange");
            running_ = false;
            return;
        }

        // Subscribe to market data
        Symbol sym(config_.trading.symbol);
        exchange_client_->subscribe_orderbook(sym, 20);
        exchange_client_->subscribe_trades(sym);
    }

    // Start worker threads
    market_data_thread_ = std::thread(&TradingEngine::market_data_thread, this);
    strategy_thread_ = std::thread(&TradingEngine::strategy_thread, this);
    order_thread_ = std::thread(&TradingEngine::order_thread, this);
    risk_thread_ = std::thread(&TradingEngine::risk_thread, this);

    // Enable strategy
    strategy_->enable();

    logger_->info("Trading engine started");
}

void TradingEngine::stop() {
    if (!running_) {
        return;
    }

    logger_->info("Stopping trading engine...");

    // Disable trading first
    trading_enabled_ = false;
    strategy_->disable();

    // Cancel all open orders
    if (exchange_client_) {
        Symbol sym(config_.trading.symbol);
        exchange_client_->cancel_all_orders(sym);
    }

    running_ = false;

    // Wait for threads to finish
    if (market_data_thread_.joinable()) market_data_thread_.join();
    if (strategy_thread_.joinable()) strategy_thread_.join();
    if (order_thread_.joinable()) order_thread_.join();
    if (risk_thread_.joinable()) risk_thread_.join();

    // Disconnect from exchange
    if (exchange_client_) {
        exchange_client_->disconnect();
    }

    logger_->info("Trading engine stopped");
    logger_->info("Statistics: Ticks=%lu, Orders=%lu, Trades=%lu",
                  ticks_processed_.load(),
                  orders_sent_.load(),
                  trades_executed_.load());
}

bool TradingEngine::is_running() const noexcept {
    return running_;
}

void TradingEngine::on_market_data(const Tick& tick) {
    if (!market_data_queue_.try_push(tick)) {
        // Queue full, drop tick (log periodically)
        static uint64_t dropped = 0;
        if (++dropped % 1000 == 0) {
            logger_->warn("Dropped %lu ticks due to queue overflow", dropped);
        }
    }
}

void TradingEngine::on_order_update(const Order& order) {
    order_queue_.try_push(order);
}

void TradingEngine::on_trade(const Trade& trade) {
    trade_queue_.try_push(trade);
}

OrderId TradingEngine::send_order(const Order& order) {
    // Pre-trade risk check
    auto risk_check = risk_manager_->check_order(order);
    if (!risk_check.passed) {
        logger_->warn("Order rejected by risk: %s", risk_check.message.c_str());
        return 0;
    }

    // Send to exchange
    if (!exchange_client_ || !trading_enabled_) {
        return 0;
    }

    OrderRequest request;
    request.symbol = order.symbol;
    request.side = order.side;
    request.type = order.type;
    request.tif = order.tif;
    request.price = order.price;
    request.quantity = order.quantity;
    request.client_order_id = ++order_id_counter_;

    auto response = exchange_client_->send_order(request);

    if (response.success) {
        risk_manager_->on_order_sent(order);
        orders_sent_++;
        return response.exchange_order_id;
    } else {
        logger_->error("Order send failed: %s", response.error_message.c_str());
        return 0;
    }
}

bool TradingEngine::cancel_order(OrderId order_id) {
    if (!exchange_client_ || order_id == 0) {
        return false;
    }

    CancelRequest request;
    request.symbol = Symbol(config_.trading.symbol);
    request.exchange_order_id = order_id;

    auto response = exchange_client_->cancel_order(request);

    if (response.success) {
        risk_manager_->on_order_canceled(order_id);
        return true;
    }

    return false;
}

bool TradingEngine::modify_order(OrderId order_id, Price new_price, Quantity new_qty) {
    // Most exchanges don't support modify - cancel and replace
    if (!cancel_order(order_id)) {
        return false;
    }

    Order new_order;
    new_order.symbol = Symbol(config_.trading.symbol);
    new_order.price = new_price;
    new_order.quantity = new_qty;
    // Copy other fields from original order...

    return send_order(new_order) != 0;
}

// ============================================================================
// Worker Threads
// ============================================================================

void TradingEngine::market_data_thread() {
    logger_->debug("Market data thread started");

    while (running_) {
        auto tick = market_data_queue_.try_pop();
        if (tick) {
            process_market_data(*tick);
        } else {
            // No data, yield CPU
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    logger_->debug("Market data thread stopped");
}

void TradingEngine::strategy_thread() {
    logger_->debug("Strategy thread started");

    while (running_) {
        if (!trading_enabled_ || !strategy_->is_enabled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Get current position
        Symbol sym(config_.trading.symbol);
        Quantity position = risk_manager_->get_position_qty(sym);

        // Generate signal (simplified)
        Signal signal;
        signal.fair_value = from_price(orderbook_->mid_price());
        signal.inventory_pressure = static_cast<double>(position) /
                                   static_cast<double>(config_.strategy.max_position);
        signal.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // Compute quotes
        auto decision = strategy_->compute_quotes(*orderbook_, position, signal);

        if (decision.should_quote) {
            // Create and send orders
            if (decision.bid_price > 0 && decision.bid_size > 0) {
                Order bid;
                bid.symbol = sym;
                bid.side = Side::BUY;
                bid.type = OrderType::LIMIT_MAKER;
                bid.tif = TimeInForce::GTX;
                bid.price = decision.bid_price;
                bid.quantity = decision.bid_size;
                send_order(bid);
            }

            if (decision.ask_price > 0 && decision.ask_size > 0) {
                Order ask;
                ask.symbol = sym;
                ask.side = Side::SELL;
                ask.type = OrderType::LIMIT_MAKER;
                ask.tif = TimeInForce::GTX;
                ask.price = decision.ask_price;
                ask.quantity = decision.ask_size;
                send_order(ask);
            }
        }

        // Rate limit strategy loop
        std::this_thread::sleep_for(
            std::chrono::microseconds(config_.strategy.quote_refresh_us));
    }

    logger_->debug("Strategy thread stopped");
}

void TradingEngine::order_thread() {
    logger_->debug("Order thread started");

    while (running_) {
        // Process order updates
        auto order = order_queue_.try_pop();
        if (order) {
            process_order_update(*order);
        }

        // Process trades
        auto trade = trade_queue_.try_pop();
        if (trade) {
            process_trade(*trade);
        }

        if (!order && !trade) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    logger_->debug("Order thread stopped");
}

void TradingEngine::risk_thread() {
    logger_->debug("Risk thread started");

    while (running_) {
        // Periodic risk checks
        double pnl = risk_manager_->get_daily_pnl();
        double exposure = risk_manager_->get_total_exposure();

        // Log periodic stats
        static uint64_t counter = 0;
        if (++counter % 100 == 0) {
            logger_->info("Risk Stats: PnL=%.2f, Exposure=%.2f, OpenOrders=%u",
                         pnl, exposure, risk_manager_->current_open_orders());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    logger_->debug("Risk thread stopped");
}

// ============================================================================
// Event Processing
// ============================================================================

void TradingEngine::process_market_data(const Tick& tick) {
    ticks_processed_++;

    // Update orderbook
    orderbook_->update_bid(tick.bid, tick.bid_qty);
    orderbook_->update_ask(tick.ask, tick.ask_qty);
    orderbook_->set_timestamp(tick.local_ts);

    // Update risk manager mark price
    Symbol sym(config_.trading.symbol);
    risk_manager_->update_mark_price(sym, orderbook_->mid_price());
}

void TradingEngine::process_order_update(const Order& order) {
    switch (order.status) {
        case OrderStatus::FILLED:
        case OrderStatus::PARTIALLY_FILLED:
            // Update will come via trade
            break;
        case OrderStatus::CANCELED:
            risk_manager_->on_order_canceled(order.id);
            strategy_->on_cancel(order.id);
            break;
        case OrderStatus::REJECTED:
            risk_manager_->on_order_rejected(order.id);
            strategy_->on_reject(order.id, "Rejected by exchange");
            break;
        default:
            break;
    }
}

void TradingEngine::process_trade(const Trade& trade) {
    trades_executed_++;

    // Update risk manager
    Order order;
    order.id = trade.order_id;
    risk_manager_->on_order_filled(order, trade.quantity, trade.price);

    // Update strategy
    strategy_->on_fill(order, trade.quantity, trade.price);
    strategy_->on_trade(trade);

    logger_->info("Trade: %s %s @ %.8f qty=%.8f",
                  trade.side == Side::BUY ? "BUY" : "SELL",
                  trade.is_maker ? "(maker)" : "(taker)",
                  from_price(trade.price),
                  from_qty(trade.quantity));
}

// ============================================================================
// EngineBuilder Implementation
// ============================================================================

EngineBuilder& EngineBuilder::with_config(const std::string& config_path) {
    config_ = ConfigParser::load(config_path);
    return *this;
}

EngineBuilder& EngineBuilder::with_symbol(const std::string& symbol) {
    config_.trading.symbol = symbol;
    return *this;
}

EngineBuilder& EngineBuilder::with_exchange(const std::string& exchange) {
    config_.exchange.name = exchange;
    return *this;
}

EngineBuilder& EngineBuilder::with_strategy(std::unique_ptr<MarketMaker> strategy) {
    strategy_ = std::move(strategy);
    return *this;
}

EngineBuilder& EngineBuilder::with_risk_limits(const RiskLimits& limits) {
    config_.risk = limits;
    return *this;
}

std::unique_ptr<TradingEngine> EngineBuilder::build() {
    return std::make_unique<TradingEngine>(config_);
}

} // namespace hft
