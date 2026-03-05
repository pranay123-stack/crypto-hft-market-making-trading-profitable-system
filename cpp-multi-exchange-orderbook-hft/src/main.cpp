/**
 * Crypto HFT System - Main Entry Point
 *
 * Institutional-grade multi-exchange cryptocurrency high-frequency trading system
 * Supports: Paper Trading, Live Trading, Backtesting
 *
 * Usage:
 *   ./crypto_hft --mode paper --config config/system.yaml
 *   ./crypto_hft --mode live --config config/system.yaml
 *   ./crypto_hft --mode backtest --config config/system.yaml --data data/
 */

#include <iostream>
#include <memory>
#include <string>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <getopt.h>

#include "core/types.hpp"
#include "core/logger.hpp"
#include "core/thread_pool.hpp"
#include "config/config_manager.hpp"
#include "config/exchange_config.hpp"
#include "config/strategy_config.hpp"
#include "exchange/exchange_factory.hpp"
#include "oms/order_manager.hpp"
#include "oms/position_manager.hpp"
#include "risk/risk_manager.hpp"
#include "risk/circuit_breaker.hpp"
#include "trading/trading_engine.hpp"
#include "trading/paper_trading.hpp"
#include "trading/live_trading.hpp"
#include "backtesting/backtest_engine.hpp"
#include "strategies/strategy_base.hpp"
#include "strategies/hft/latency_arbitrage.hpp"
#include "strategies/hft/triangular_arbitrage.hpp"
#include "strategies/hft/statistical_arbitrage.hpp"
#include "strategies/hft/momentum_ignition.hpp"
#include "strategies/hft/order_flow_imbalance.hpp"
#include "strategies/market_making/basic_market_maker.hpp"
#include "strategies/market_making/adaptive_market_maker.hpp"
#include "strategies/market_making/inventory_market_maker.hpp"
#include "strategies/market_making/grid_market_maker.hpp"
#include "strategies/market_making/avellaneda_stoikov.hpp"

namespace hft {

// Global shutdown flag
std::atomic<bool> g_shutdown{false};

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    std::cout << "\n[SIGNAL] Received signal " << signal << ", initiating graceful shutdown...\n";
    g_shutdown.store(true, std::memory_order_release);
}

// Print usage information
void print_usage(const char* program_name) {
    std::cout << R"(
Crypto HFT System - Institutional Grade Multi-Exchange Trading

Usage: )" << program_name << R"( [OPTIONS]

Options:
  -m, --mode <mode>       Trading mode: paper, live, backtest (default: paper)
  -c, --config <path>     Path to system configuration file (default: config/system.yaml)
  -e, --exchanges <path>  Path to exchanges configuration (default: config/exchanges.yaml)
  -s, --strategies <path> Path to strategies configuration (default: config/strategies.yaml)
  -r, --risk <path>       Path to risk configuration (default: config/risk.yaml)
  -d, --data <path>       Path to historical data directory (for backtest mode)
  -v, --verbose           Enable verbose logging
  -h, --help              Show this help message

Examples:
  Paper Trading:
    ./crypto_hft --mode paper

  Live Trading:
    ./crypto_hft --mode live --config config/system.yaml

  Backtesting:
    ./crypto_hft --mode backtest --data data/2024/

Supported Exchanges (10):
  Binance, Bybit, OKX, Kraken, Coinbase, KuCoin, Gate.io, Bitfinex, Deribit, HTX

Strategies (10):
  HFT Strategies:
    - Latency Arbitrage
    - Triangular Arbitrage
    - Statistical Arbitrage
    - Momentum Ignition
    - Order Flow Imbalance

  Market Making Strategies:
    - Basic Market Maker
    - Adaptive Market Maker
    - Inventory-Aware Market Maker
    - Grid Market Maker
    - Avellaneda-Stoikov Optimal MM

)" << std::endl;
}

// Parse command line arguments
struct CommandLineArgs {
    TradingMode mode = TradingMode::Paper;
    std::string config_path = "config/system.yaml";
    std::string exchanges_config = "config/exchanges.yaml";
    std::string strategies_config = "config/strategies.yaml";
    std::string risk_config = "config/risk.yaml";
    std::string data_path = "";
    bool verbose = false;
};

CommandLineArgs parse_args(int argc, char* argv[]) {
    CommandLineArgs args;

    static struct option long_options[] = {
        {"mode",       required_argument, nullptr, 'm'},
        {"config",     required_argument, nullptr, 'c'},
        {"exchanges",  required_argument, nullptr, 'e'},
        {"strategies", required_argument, nullptr, 's'},
        {"risk",       required_argument, nullptr, 'r'},
        {"data",       required_argument, nullptr, 'd'},
        {"verbose",    no_argument,       nullptr, 'v'},
        {"help",       no_argument,       nullptr, 'h'},
        {nullptr,      0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "m:c:e:s:r:d:vh", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'm':
                if (std::string(optarg) == "paper") {
                    args.mode = TradingMode::Paper;
                } else if (std::string(optarg) == "live") {
                    args.mode = TradingMode::Live;
                } else if (std::string(optarg) == "backtest") {
                    args.mode = TradingMode::Backtest;
                } else {
                    std::cerr << "Invalid mode: " << optarg << std::endl;
                    exit(1);
                }
                break;
            case 'c':
                args.config_path = optarg;
                break;
            case 'e':
                args.exchanges_config = optarg;
                break;
            case 's':
                args.strategies_config = optarg;
                break;
            case 'r':
                args.risk_config = optarg;
                break;
            case 'd':
                args.data_path = optarg;
                break;
            case 'v':
                args.verbose = true;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                exit(1);
        }
    }

    return args;
}

// Main application class
class CryptoHFTApplication {
public:
    CryptoHFTApplication(const CommandLineArgs& args)
        : args_(args)
        , logger_(std::make_shared<core::Logger>("CryptoHFT"))
        , thread_pool_(std::make_shared<core::ThreadPool>(std::thread::hardware_concurrency()))
    {}

    bool initialize() {
        logger_->info("Initializing Crypto HFT System...");

        // Load configurations
        if (!load_configurations()) {
            logger_->error("Failed to load configurations");
            return false;
        }

        // Initialize components based on mode
        if (!initialize_components()) {
            logger_->error("Failed to initialize components");
            return false;
        }

        // Register strategies
        if (!register_strategies()) {
            logger_->error("Failed to register strategies");
            return false;
        }

        logger_->info("Initialization complete. Mode: {}", trading_mode_to_string(args_.mode));
        return true;
    }

    void run() {
        logger_->info("Starting trading engine...");

        switch (args_.mode) {
            case TradingMode::Paper:
                run_paper_trading();
                break;
            case TradingMode::Live:
                run_live_trading();
                break;
            case TradingMode::Backtest:
                run_backtesting();
                break;
        }
    }

    void shutdown() {
        logger_->info("Shutting down...");

        // Stop all strategies
        if (trading_engine_) {
            trading_engine_->stop();
        }

        // Close exchange connections
        for (auto& [name, exchange] : exchanges_) {
            exchange->disconnect();
        }

        // Shutdown thread pool
        thread_pool_->shutdown();

        logger_->info("Shutdown complete.");
    }

private:
    bool load_configurations() {
        try {
            config_manager_ = std::make_shared<config::ConfigManager>();

            // Load system config
            if (!config_manager_->load(args_.config_path)) {
                logger_->warn("Could not load system config from {}, using defaults", args_.config_path);
            }

            // Load exchange configs
            exchange_config_ = std::make_shared<config::ExchangeConfig>();
            if (!exchange_config_->load(args_.exchanges_config)) {
                logger_->warn("Could not load exchange config from {}, using defaults", args_.exchanges_config);
            }

            // Load strategy configs
            strategy_config_ = std::make_shared<config::StrategyConfig>();
            if (!strategy_config_->load(args_.strategies_config)) {
                logger_->warn("Could not load strategy config from {}, using defaults", args_.strategies_config);
            }

            return true;
        } catch (const std::exception& e) {
            logger_->error("Configuration loading error: {}", e.what());
            return false;
        }
    }

    bool initialize_components() {
        try {
            // Initialize OMS
            order_manager_ = std::make_shared<oms::OrderManager>();
            position_manager_ = std::make_shared<oms::PositionManager>();

            // Initialize Risk Management
            risk_manager_ = std::make_shared<risk::RiskManager>();
            circuit_breaker_ = std::make_shared<risk::CircuitBreaker>();

            // Initialize exchanges
            auto enabled_exchanges = exchange_config_->get_enabled_exchanges();
            for (const auto& exchange_name : enabled_exchanges) {
                auto exchange = exchange::ExchangeFactory::create(
                    exchange_name,
                    exchange_config_->get_config(exchange_name)
                );
                if (exchange) {
                    exchanges_[exchange_name] = std::move(exchange);
                    logger_->info("Initialized exchange: {}", exchange_name);
                }
            }

            if (exchanges_.empty()) {
                logger_->warn("No exchanges enabled, using default Binance for demo");
                // Create default exchange for demo purposes
            }

            // Initialize trading engine
            trading_engine_ = std::make_shared<trading::TradingEngine>(
                order_manager_,
                position_manager_,
                risk_manager_,
                args_.mode
            );

            return true;
        } catch (const std::exception& e) {
            logger_->error("Component initialization error: {}", e.what());
            return false;
        }
    }

    bool register_strategies() {
        try {
            // Register HFT strategies
            if (strategy_config_->is_enabled("latency_arbitrage")) {
                auto params = strategy_config_->get_params("latency_arbitrage");
                auto strategy = std::make_unique<strategies::hft::LatencyArbitrage>(params);
                trading_engine_->register_strategy(std::move(strategy));
                logger_->info("Registered: Latency Arbitrage");
            }

            if (strategy_config_->is_enabled("triangular_arbitrage")) {
                auto params = strategy_config_->get_params("triangular_arbitrage");
                auto strategy = std::make_unique<strategies::hft::TriangularArbitrage>(params);
                trading_engine_->register_strategy(std::move(strategy));
                logger_->info("Registered: Triangular Arbitrage");
            }

            if (strategy_config_->is_enabled("statistical_arbitrage")) {
                auto params = strategy_config_->get_params("statistical_arbitrage");
                auto strategy = std::make_unique<strategies::hft::StatisticalArbitrage>(params);
                trading_engine_->register_strategy(std::move(strategy));
                logger_->info("Registered: Statistical Arbitrage");
            }

            if (strategy_config_->is_enabled("momentum_ignition")) {
                auto params = strategy_config_->get_params("momentum_ignition");
                auto strategy = std::make_unique<strategies::hft::MomentumIgnition>(params);
                trading_engine_->register_strategy(std::move(strategy));
                logger_->info("Registered: Momentum Ignition");
            }

            if (strategy_config_->is_enabled("order_flow_imbalance")) {
                auto params = strategy_config_->get_params("order_flow_imbalance");
                auto strategy = std::make_unique<strategies::hft::OrderFlowImbalance>(params);
                trading_engine_->register_strategy(std::move(strategy));
                logger_->info("Registered: Order Flow Imbalance");
            }

            // Register Market Making strategies
            if (strategy_config_->is_enabled("basic_market_maker")) {
                auto params = strategy_config_->get_params("basic_market_maker");
                auto strategy = std::make_unique<strategies::mm::BasicMarketMaker>(params);
                trading_engine_->register_strategy(std::move(strategy));
                logger_->info("Registered: Basic Market Maker");
            }

            if (strategy_config_->is_enabled("adaptive_market_maker")) {
                auto params = strategy_config_->get_params("adaptive_market_maker");
                auto strategy = std::make_unique<strategies::mm::AdaptiveMarketMaker>(params);
                trading_engine_->register_strategy(std::move(strategy));
                logger_->info("Registered: Adaptive Market Maker");
            }

            if (strategy_config_->is_enabled("inventory_market_maker")) {
                auto params = strategy_config_->get_params("inventory_market_maker");
                auto strategy = std::make_unique<strategies::mm::InventoryMarketMaker>(params);
                trading_engine_->register_strategy(std::move(strategy));
                logger_->info("Registered: Inventory Market Maker");
            }

            if (strategy_config_->is_enabled("grid_market_maker")) {
                auto params = strategy_config_->get_params("grid_market_maker");
                auto strategy = std::make_unique<strategies::mm::GridMarketMaker>(params);
                trading_engine_->register_strategy(std::move(strategy));
                logger_->info("Registered: Grid Market Maker");
            }

            if (strategy_config_->is_enabled("avellaneda_stoikov")) {
                auto params = strategy_config_->get_params("avellaneda_stoikov");
                auto strategy = std::make_unique<strategies::mm::AvellanedaStoikov>(params);
                trading_engine_->register_strategy(std::move(strategy));
                logger_->info("Registered: Avellaneda-Stoikov MM");
            }

            return true;
        } catch (const std::exception& e) {
            logger_->error("Strategy registration error: {}", e.what());
            return false;
        }
    }

    void run_paper_trading() {
        logger_->info("Starting Paper Trading Mode");

        auto paper_trading = std::make_shared<trading::PaperTrading>(
            exchanges_,
            order_manager_,
            position_manager_
        );

        trading_engine_->set_trading_mode(paper_trading);

        // Connect to exchanges for market data
        for (auto& [name, exchange] : exchanges_) {
            exchange->connect();
            // Subscribe to market data for configured symbols
            auto symbols = exchange_config_->get_trading_pairs(name);
            for (const auto& symbol : symbols) {
                exchange->subscribe_orderbook(symbol);
                exchange->subscribe_trades(symbol);
            }
        }

        // Start trading engine
        trading_engine_->start();

        // Main loop
        while (!g_shutdown.load(std::memory_order_acquire)) {
            trading_engine_->process_events();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    void run_live_trading() {
        logger_->info("Starting Live Trading Mode");
        logger_->warn("LIVE TRADING - Real money at risk!");

        // Additional safety confirmation
        std::cout << "\n!!! WARNING: LIVE TRADING MODE !!!\n";
        std::cout << "Real orders will be placed on exchanges.\n";
        std::cout << "Type 'CONFIRM' to proceed: ";
        std::string confirmation;
        std::getline(std::cin, confirmation);

        if (confirmation != "CONFIRM") {
            logger_->info("Live trading cancelled by user");
            return;
        }

        auto live_trading = std::make_shared<trading::LiveTrading>(
            exchanges_,
            order_manager_,
            position_manager_,
            risk_manager_
        );

        trading_engine_->set_trading_mode(live_trading);

        // Connect to exchanges
        for (auto& [name, exchange] : exchanges_) {
            if (!exchange->connect()) {
                logger_->error("Failed to connect to {}", name);
                return;
            }

            // Sync balances
            exchange->sync_balances();

            // Subscribe to market data
            auto symbols = exchange_config_->get_trading_pairs(name);
            for (const auto& symbol : symbols) {
                exchange->subscribe_orderbook(symbol);
                exchange->subscribe_trades(symbol);
                exchange->subscribe_user_data();  // For order updates
            }
        }

        // Start trading engine with circuit breaker
        circuit_breaker_->arm();
        trading_engine_->start();

        // Main loop with additional monitoring
        while (!g_shutdown.load(std::memory_order_acquire)) {
            // Check circuit breaker
            if (circuit_breaker_->is_triggered()) {
                logger_->error("Circuit breaker triggered! Stopping trading.");
                trading_engine_->emergency_stop();
                break;
            }

            trading_engine_->process_events();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        // Graceful shutdown - cancel all open orders
        logger_->info("Cancelling all open orders...");
        order_manager_->cancel_all_orders();
    }

    void run_backtesting() {
        logger_->info("Starting Backtesting Mode");

        if (args_.data_path.empty()) {
            logger_->error("Data path required for backtesting. Use --data <path>");
            return;
        }

        auto backtest_engine = std::make_shared<backtesting::BacktestEngine>(
            args_.data_path,
            order_manager_,
            position_manager_
        );

        // Register strategies with backtest engine
        for (auto& strategy : trading_engine_->get_strategies()) {
            backtest_engine->register_strategy(strategy);
        }

        // Run backtest
        logger_->info("Loading historical data from: {}", args_.data_path);
        backtest_engine->load_data();

        logger_->info("Running backtest...");
        auto start_time = std::chrono::high_resolution_clock::now();

        backtest_engine->run();

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // Generate performance report
        auto report = backtest_engine->generate_report();

        logger_->info("Backtest completed in {} ms", duration.count());
        logger_->info("\n{}", report.to_string());

        // Save report to file
        report.save_to_file("backtest_results.json");
    }

    static std::string trading_mode_to_string(TradingMode mode) {
        switch (mode) {
            case TradingMode::Paper: return "Paper";
            case TradingMode::Live: return "Live";
            case TradingMode::Backtest: return "Backtest";
            default: return "Unknown";
        }
    }

private:
    CommandLineArgs args_;
    std::shared_ptr<core::Logger> logger_;
    std::shared_ptr<core::ThreadPool> thread_pool_;
    std::shared_ptr<config::ConfigManager> config_manager_;
    std::shared_ptr<config::ExchangeConfig> exchange_config_;
    std::shared_ptr<config::StrategyConfig> strategy_config_;
    std::shared_ptr<oms::OrderManager> order_manager_;
    std::shared_ptr<oms::PositionManager> position_manager_;
    std::shared_ptr<risk::RiskManager> risk_manager_;
    std::shared_ptr<risk::CircuitBreaker> circuit_breaker_;
    std::shared_ptr<trading::TradingEngine> trading_engine_;
    std::unordered_map<std::string, std::shared_ptr<exchange::ExchangeBase>> exchanges_;
};

}  // namespace hft

int main(int argc, char* argv[]) {
    // Setup signal handlers
    std::signal(SIGINT, hft::signal_handler);
    std::signal(SIGTERM, hft::signal_handler);

    // Print banner
    std::cout << R"(
   ____                  _          _   _ _____ _____
  / ___|_ __ _   _ _ __ | |_ ___   | | | |  ___|_   _|
 | |   | '__| | | | '_ \| __/ _ \  | |_| | |_    | |
 | |___| |  | |_| | |_) | || (_) | |  _  |  _|   | |
  \____|_|   \__, | .__/ \__\___/  |_| |_|_|     |_|
             |___/|_|

  Institutional Grade Multi-Exchange Trading System
  Version 1.0.0 | 10 Exchanges | 10 Strategies
    )" << std::endl;

    // Parse command line arguments
    auto args = hft::parse_args(argc, argv);

    // Create and run application
    hft::CryptoHFTApplication app(args);

    if (!app.initialize()) {
        std::cerr << "Failed to initialize application" << std::endl;
        return 1;
    }

    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    app.shutdown();

    return 0;
}
