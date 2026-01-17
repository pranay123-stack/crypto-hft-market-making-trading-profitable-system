#include "core/engine.hpp"
#include "utils/config.hpp"
#include "utils/logger.hpp"

#include <iostream>
#include <csignal>
#include <atomic>

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

void print_banner() {
    std::cout << R"(
╔═══════════════════════════════════════════════════════════════╗
║          Crypto HFT Market Making System v1.0.0               ║
║          High-Frequency Trading Bot - Single Exchange         ║
╚═══════════════════════════════════════════════════════════════╝
)" << std::endl;
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "Options:\n"
              << "  -c, --config <file>    Configuration file path (default: config/config.json)\n"
              << "  -s, --symbol <symbol>  Trading symbol (e.g., BTCUSDT)\n"
              << "  -e, --exchange <name>  Exchange name (e.g., binance)\n"
              << "  -t, --testnet          Use testnet\n"
              << "  -p, --paper            Paper trading mode\n"
              << "  -v, --verbose          Verbose logging\n"
              << "  -h, --help             Show this help message\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    print_banner();

    // Parse command line arguments
    std::string config_path = "config/config.json";
    std::string symbol;
    std::string exchange;
    bool testnet = false;
    bool paper = true;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((arg == "-s" || arg == "--symbol") && i + 1 < argc) {
            symbol = argv[++i];
        } else if ((arg == "-e" || arg == "--exchange") && i + 1 < argc) {
            exchange = argv[++i];
        } else if (arg == "-t" || arg == "--testnet") {
            testnet = true;
        } else if (arg == "-p" || arg == "--paper") {
            paper = true;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        }
    }

    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        // Load configuration
        LOG_INFO("Loading configuration from %s", config_path.c_str());
        hft::Config config;

        try {
            config = hft::ConfigParser::load(config_path);
        } catch (const std::exception& e) {
            LOG_WARN("Failed to load config file: %s, using defaults", e.what());
            config = hft::defaults::binance_spot_config();
        }

        // Apply command line overrides
        if (!symbol.empty()) {
            config.trading.symbol = symbol;
        }
        if (!exchange.empty()) {
            config.exchange.name = exchange;
        }
        config.trading.paper_trading = paper;
        if (verbose) {
            config.system.log_level = "DEBUG";
        }

        // Apply environment variable overrides
        hft::ConfigParser::apply_env_overrides(config);

        // Validate configuration
        std::string validation_error;
        if (!hft::ConfigParser::validate(config, validation_error)) {
            LOG_ERROR("Configuration validation failed: %s", validation_error.c_str());
            return 1;
        }

        // Print configuration summary
        LOG_INFO("Configuration Summary:");
        LOG_INFO("  Exchange: %s", config.exchange.name.c_str());
        LOG_INFO("  Symbol: %s", config.trading.symbol.c_str());
        LOG_INFO("  Mode: %s", config.trading.paper_trading ? "Paper Trading" : "LIVE TRADING");
        LOG_INFO("  Spread: %.1f - %.1f bps", config.strategy.min_spread_bps, config.strategy.max_spread_bps);

        // Build and start engine
        LOG_INFO("Initializing trading engine...");

        auto engine = hft::EngineBuilder()
            .with_config(config_path)
            .with_symbol(config.trading.symbol)
            .with_exchange(config.exchange.name)
            .with_risk_limits(config.risk)
            .build();

        if (!engine) {
            LOG_ERROR("Failed to create trading engine");
            return 1;
        }

        LOG_INFO("Starting trading engine...");
        engine->start();

        LOG_INFO("Engine started. Press Ctrl+C to stop.");

        // Main loop
        while (g_running && engine->is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Shutdown
        LOG_INFO("Stopping trading engine...");
        engine->stop();

        LOG_INFO("Shutdown complete.");

    } catch (const std::exception& e) {
        LOG_FATAL("Fatal error: %s", e.what());
        return 1;
    }

    return 0;
}
