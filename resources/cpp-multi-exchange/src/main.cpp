#include "core/types.hpp"
#include "exchange/exchange_manager.hpp"
#include "orderbook/consolidated_book.hpp"
#include "strategy/cross_exchange_mm.hpp"
#include "arbitrage/arbitrage_detector.hpp"

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

void print_banner() {
    std::cout << R"(
╔═══════════════════════════════════════════════════════════════════════╗
║        Crypto HFT Market Making System v1.0.0 - Multi Exchange        ║
║        High-Frequency Trading Bot with Cross-Exchange Arbitrage       ║
╚═══════════════════════════════════════════════════════════════════════╝
)" << std::endl;
}

int main(int argc, char* argv[]) {
    print_banner();

    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        // ================================================================
        // Initialize Exchange Manager
        // ================================================================
        std::cout << "[INFO] Initializing exchange manager..." << std::endl;

        hft::ExchangeManager exchange_manager;

        // Configure Binance
        hft::ExchangeConfig binance_config;
        binance_config.id = hft::ExchangeId::BINANCE;
        binance_config.name = "binance";
        binance_config.rest_url = "https://testnet.binance.vision";
        binance_config.ws_url = "wss://testnet.binance.vision/ws";
        binance_config.api_key = std::getenv("BINANCE_API_KEY") ? std::getenv("BINANCE_API_KEY") : "";
        binance_config.api_secret = std::getenv("BINANCE_API_SECRET") ? std::getenv("BINANCE_API_SECRET") : "";
        binance_config.priority = 1;
        exchange_manager.add_exchange(binance_config);

        // Configure Bybit
        hft::ExchangeConfig bybit_config;
        bybit_config.id = hft::ExchangeId::BYBIT;
        bybit_config.name = "bybit";
        bybit_config.rest_url = "https://api-testnet.bybit.com";
        bybit_config.ws_url = "wss://stream-testnet.bybit.com/v5/public/spot";
        bybit_config.api_key = std::getenv("BYBIT_API_KEY") ? std::getenv("BYBIT_API_KEY") : "";
        bybit_config.api_secret = std::getenv("BYBIT_API_SECRET") ? std::getenv("BYBIT_API_SECRET") : "";
        bybit_config.priority = 2;
        exchange_manager.add_exchange(bybit_config);

        // Configure OKX
        hft::ExchangeConfig okx_config;
        okx_config.id = hft::ExchangeId::OKX;
        okx_config.name = "okx";
        okx_config.rest_url = "https://www.okx.com";
        okx_config.ws_url = "wss://ws.okx.com:8443/ws/v5/public";
        okx_config.api_key = std::getenv("OKX_API_KEY") ? std::getenv("OKX_API_KEY") : "";
        okx_config.api_secret = std::getenv("OKX_API_SECRET") ? std::getenv("OKX_API_SECRET") : "";
        okx_config.passphrase = std::getenv("OKX_PASSPHRASE") ? std::getenv("OKX_PASSPHRASE") : "";
        okx_config.priority = 3;
        exchange_manager.add_exchange(okx_config);

        // ================================================================
        // Initialize Consolidated Order Book
        // ================================================================
        std::cout << "[INFO] Initializing consolidated order book..." << std::endl;

        hft::Symbol symbol("BTCUSDT");
        hft::ConsolidatedBook consolidated_book(symbol);

        // ================================================================
        // Initialize Cross-Exchange Market Maker
        // ================================================================
        std::cout << "[INFO] Initializing cross-exchange market maker..." << std::endl;

        hft::CrossExchangeMMParams mm_params;
        mm_params.min_spread_bps = 5.0;
        mm_params.max_spread_bps = 100.0;
        mm_params.target_spread_bps = 15.0;
        mm_params.max_position_per_exchange = hft::to_qty(0.1);
        mm_params.max_total_position = hft::to_qty(0.2);
        mm_params.default_order_size = hft::to_qty(0.001);
        mm_params.min_order_size = hft::to_qty(0.0001);
        mm_params.max_order_size = hft::to_qty(0.01);
        mm_params.quote_exchanges = {
            hft::ExchangeId::BINANCE,
            hft::ExchangeId::BYBIT,
            hft::ExchangeId::OKX
        };
        mm_params.hedge_exchanges = {
            hft::ExchangeId::BINANCE,
            hft::ExchangeId::BYBIT
        };

        hft::CrossExchangeMarketMaker strategy(mm_params);

        // ================================================================
        // Initialize Arbitrage Detector
        // ================================================================
        std::cout << "[INFO] Initializing arbitrage detector..." << std::endl;

        hft::ArbitrageConfig arb_config;
        arb_config.min_profit_bps = 5.0;
        arb_config.max_slippage_bps = 2.0;
        arb_config.min_quantity = hft::to_qty(0.001);
        arb_config.max_quantity = hft::to_qty(0.1);

        hft::ArbitrageDetector arb_detector(arb_config);

        arb_detector.set_opportunity_callback([](const hft::ArbitrageOpportunity& opp) {
            std::cout << "[ARB] Opportunity detected: "
                      << "Buy on " << hft::exchange_name(opp.buy_exchange)
                      << " @ " << hft::from_price(opp.buy_price)
                      << ", Sell on " << hft::exchange_name(opp.sell_exchange)
                      << " @ " << hft::from_price(opp.sell_price)
                      << ", Profit: " << opp.profit_bps << " bps"
                      << std::endl;
        });

        // ================================================================
        // Setup Callbacks
        // ================================================================
        hft::ExchangeCallbacks callbacks;

        callbacks.on_tick = [&](hft::ExchangeId exchange, const hft::Tick& tick) {
            // Update consolidated book
            consolidated_book.update_bid(exchange, tick.bid, tick.bid_qty);
            consolidated_book.update_ask(exchange, tick.ask, tick.ask_qty);

            // Check for arbitrage
            arb_detector.on_book_update(consolidated_book);
        };

        callbacks.on_trade = [&](hft::ExchangeId exchange, const hft::Trade& trade) {
            std::cout << "[TRADE] " << hft::exchange_name(exchange)
                      << " " << (trade.side == hft::Side::BUY ? "BUY" : "SELL")
                      << " @ " << hft::from_price(trade.price)
                      << " qty=" << hft::from_qty(trade.quantity)
                      << std::endl;
        };

        callbacks.on_connected = [](hft::ExchangeId exchange) {
            std::cout << "[INFO] Connected to " << hft::exchange_name(exchange) << std::endl;
        };

        callbacks.on_disconnected = [](hft::ExchangeId exchange) {
            std::cout << "[WARN] Disconnected from " << hft::exchange_name(exchange) << std::endl;
        };

        callbacks.on_error = [](hft::ExchangeId exchange, const std::string& error) {
            std::cerr << "[ERROR] " << hft::exchange_name(exchange) << ": " << error << std::endl;
        };

        exchange_manager.set_callbacks(callbacks);

        // ================================================================
        // Connect to Exchanges
        // ================================================================
        std::cout << "[INFO] Connecting to exchanges..." << std::endl;
        exchange_manager.connect_all();

        // Subscribe to market data
        std::cout << "[INFO] Subscribing to market data..." << std::endl;
        exchange_manager.subscribe_all(symbol);

        // Enable strategy
        strategy.enable();

        std::cout << "[INFO] System started. Press Ctrl+C to stop." << std::endl;
        std::cout << "[INFO] Monitoring " << symbol.str() << " across "
                  << exchange_manager.exchange_count() << " exchanges" << std::endl;

        // ================================================================
        // Main Loop
        // ================================================================
        hft::CrossExchangePosition position;
        uint64_t loop_count = 0;

        while (g_running) {
            loop_count++;

            // Compute and potentially send quotes
            if (strategy.is_enabled()) {
                auto decisions = strategy.compute_quotes(consolidated_book, position);

                // Process quote decisions
                for (const auto& quote : decisions.quotes) {
                    if (quote.should_quote) {
                        // Would send orders here
                        // strategy.send_order(...)
                    }
                }
            }

            // Print periodic stats
            if (loop_count % 100 == 0) {
                auto nbbo = consolidated_book.get_nbbo();
                if (nbbo.is_valid()) {
                    std::cout << "[NBBO] Bid: " << hft::from_price(nbbo.best_bid)
                              << " (" << hft::exchange_name(nbbo.best_bid_exchange) << ")"
                              << " Ask: " << hft::from_price(nbbo.best_ask)
                              << " (" << hft::exchange_name(nbbo.best_ask_exchange) << ")"
                              << " Spread: " << nbbo.spread_bps() << " bps"
                              << std::endl;
                }

                std::cout << "[STATS] Arb opportunities: "
                          << arb_detector.opportunities_detected()
                          << ", Quotes: " << strategy.total_quotes()
                          << ", Fills: " << strategy.total_fills()
                          << std::endl;
            }

            // Rate limit main loop
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // ================================================================
        // Shutdown
        // ================================================================
        std::cout << "[INFO] Shutting down..." << std::endl;

        strategy.disable();
        exchange_manager.disconnect_all();

        std::cout << "[INFO] Final Statistics:" << std::endl;
        std::cout << "  - Arbitrage opportunities detected: "
                  << arb_detector.opportunities_detected() << std::endl;
        std::cout << "  - Total quotes: " << strategy.total_quotes() << std::endl;
        std::cout << "  - Total fills: " << strategy.total_fills() << std::endl;
        std::cout << "  - Hedge orders: " << strategy.hedge_orders() << std::endl;

        std::cout << "[INFO] Shutdown complete." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
