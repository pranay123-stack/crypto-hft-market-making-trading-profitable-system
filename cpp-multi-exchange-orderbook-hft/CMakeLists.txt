cmake_minimum_required(VERSION 3.20)
project(crypto_hft_system VERSION 1.0.0 LANGUAGES CXX)

# C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags for performance
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -O3
        -march=native
        -ffast-math
        -funroll-loops
    )

    # Link-time optimization for Release
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-flto)
        add_link_options(-flto)
    endif()
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Find Dependencies
# =============================================================================

find_package(Threads REQUIRED)
find_package(OpenSSL QUIET)
find_package(CURL QUIET)

# Optional: Boost
find_package(Boost 1.75 QUIET COMPONENTS system)

# Optional: yaml-cpp
find_package(yaml-cpp QUIET)

# Optional: nlohmann_json
find_package(nlohmann_json QUIET)

# =============================================================================
# Core Library
# =============================================================================

add_library(hft_core STATIC
    src/core/lock_free_queue.cpp
    src/core/memory_pool.cpp
    src/core/timestamp.cpp
    src/core/logger.cpp
    src/core/thread_pool.cpp
)

target_include_directories(hft_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hft_core PUBLIC Threads::Threads)

# =============================================================================
# Utils Library
# =============================================================================

add_library(hft_utils STATIC
    src/utils/crypto_utils.cpp
    src/utils/string_utils.cpp
    src/utils/math_utils.cpp
)

target_link_libraries(hft_utils PUBLIC hft_core)
if(OpenSSL_FOUND)
    target_link_libraries(hft_utils PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# =============================================================================
# Network Library
# =============================================================================

add_library(hft_network STATIC
    src/network/websocket_client.cpp
    src/network/rest_client.cpp
    src/network/connection_pool.cpp
)

target_link_libraries(hft_network PUBLIC hft_core hft_utils)
if(OpenSSL_FOUND)
    target_link_libraries(hft_network PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
if(CURL_FOUND)
    target_link_libraries(hft_network PUBLIC CURL::libcurl)
endif()
if(Boost_FOUND)
    target_link_libraries(hft_network PUBLIC Boost::system)
endif()

# =============================================================================
# Exchange Library
# =============================================================================

add_library(hft_exchange STATIC
    src/exchange/exchange_base.cpp
    src/exchange/exchange_factory.cpp
    src/exchange/binance.cpp
    src/exchange/bybit.cpp
    src/exchange/okx.cpp
    src/exchange/kraken.cpp
    src/exchange/coinbase.cpp
    src/exchange/kucoin.cpp
    src/exchange/gateio.cpp
    src/exchange/bitfinex.cpp
    src/exchange/deribit.cpp
    src/exchange/htx.cpp
)

target_link_libraries(hft_exchange PUBLIC hft_core hft_network hft_utils)

# =============================================================================
# OMS Library
# =============================================================================

add_library(hft_oms STATIC
    src/oms/order.cpp
    src/oms/order_manager.cpp
    src/oms/position_manager.cpp
    src/oms/execution_engine.cpp
)

target_link_libraries(hft_oms PUBLIC hft_core hft_exchange)

# =============================================================================
# Risk Library
# =============================================================================

add_library(hft_risk STATIC
    src/risk/risk_manager.cpp
    src/risk/position_limits.cpp
    src/risk/pnl_tracker.cpp
    src/risk/circuit_breaker.cpp
)

target_link_libraries(hft_risk PUBLIC hft_core hft_oms)

# =============================================================================
# Strategies Library
# =============================================================================

add_library(hft_strategies STATIC
    # HFT Strategies
    src/strategies/hft/latency_arbitrage.cpp
    src/strategies/hft/triangular_arbitrage.cpp
    src/strategies/hft/statistical_arbitrage.cpp
    src/strategies/hft/momentum_ignition.cpp
    src/strategies/hft/order_flow_imbalance.cpp
    # Market Making Strategies
    src/strategies/market_making/basic_market_maker.cpp
    src/strategies/market_making/adaptive_market_maker.cpp
    src/strategies/market_making/inventory_market_maker.cpp
    src/strategies/market_making/grid_market_maker.cpp
    src/strategies/market_making/avellaneda_stoikov.cpp
)

target_link_libraries(hft_strategies PUBLIC hft_core hft_oms hft_risk)

# =============================================================================
# Backtesting Library
# =============================================================================

add_library(hft_backtesting STATIC
    src/backtesting/data_feed.cpp
    src/backtesting/simulated_exchange.cpp
    src/backtesting/performance_analyzer.cpp
    src/backtesting/backtest_engine.cpp
)

target_link_libraries(hft_backtesting PUBLIC hft_core hft_strategies hft_oms)

# =============================================================================
# Trading Library
# =============================================================================

add_library(hft_trading STATIC
    src/trading/paper_trading.cpp
    src/trading/live_trading.cpp
    src/trading/trading_engine.cpp
)

target_link_libraries(hft_trading PUBLIC hft_core hft_strategies hft_exchange hft_oms hft_risk)

# =============================================================================
# Config Library
# =============================================================================

add_library(hft_config STATIC
    src/config/config_manager.cpp
    src/config/exchange_config.cpp
    src/config/strategy_config.cpp
)

target_link_libraries(hft_config PUBLIC hft_core)
if(yaml-cpp_FOUND)
    target_link_libraries(hft_config PUBLIC yaml-cpp)
endif()

# =============================================================================
# Main Executable
# =============================================================================

add_executable(crypto_hft
    src/main.cpp
)

target_link_libraries(crypto_hft PRIVATE
    hft_core
    hft_utils
    hft_network
    hft_exchange
    hft_oms
    hft_risk
    hft_strategies
    hft_backtesting
    hft_trading
    hft_config
)

# =============================================================================
# Tests (optional)
# =============================================================================

option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        enable_testing()

        add_executable(test_core tests/test_core.cpp)
        target_link_libraries(test_core PRIVATE hft_core GTest::gtest GTest::gtest_main)
        add_test(NAME test_core COMMAND test_core)

        add_executable(test_strategies tests/test_strategies.cpp)
        target_link_libraries(test_strategies PRIVATE hft_strategies hft_core GTest::gtest GTest::gtest_main)
        add_test(NAME test_strategies COMMAND test_strategies)
    endif()
endif()

# =============================================================================
# Benchmarks (optional)
# =============================================================================

option(BUILD_BENCHMARKS "Build benchmarks" OFF)
if(BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_executable(benchmark_core benchmarks/benchmark_core.cpp)
        target_link_libraries(benchmark_core PRIVATE hft_core benchmark::benchmark)

        add_executable(benchmark_strategies benchmarks/benchmark_strategies.cpp)
        target_link_libraries(benchmark_strategies PRIVATE hft_strategies hft_core benchmark::benchmark)
    endif()
endif()

# =============================================================================
# Installation
# =============================================================================

install(TARGETS crypto_hft DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/crypto_hft)
install(DIRECTORY include/ DESTINATION include/crypto_hft)

# Print configuration summary
message(STATUS "")
message(STATUS "=== Crypto HFT System Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "OpenSSL: ${OpenSSL_FOUND}")
message(STATUS "CURL: ${CURL_FOUND}")
message(STATUS "Boost: ${Boost_FOUND}")
message(STATUS "yaml-cpp: ${yaml-cpp_FOUND}")
message(STATUS "nlohmann_json: ${nlohmann_json_FOUND}")
message(STATUS "Tests: ${BUILD_TESTS}")
message(STATUS "Benchmarks: ${BUILD_BENCHMARKS}")
message(STATUS "=========================================")
message(STATUS "")
