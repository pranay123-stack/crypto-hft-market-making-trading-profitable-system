cmake_minimum_required(VERSION 3.20)
project(crypto_hft_market_maker VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags for HFT optimization
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -flto -DNDEBUG")

# HFT-specific optimizations
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math -funroll-loops")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-exceptions -fno-rtti")

# Find required packages
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(Boost 1.75 REQUIRED COMPONENTS system)

# Optional: Find low-latency libraries
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBURING liburing)
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
)

# Source files
set(CORE_SOURCES
    src/core/engine.cpp
    src/core/event_loop.cpp
    src/core/memory_pool.cpp
    src/core/lock_free_queue.cpp
    src/core/timestamp.cpp
)

set(EXCHANGE_SOURCES
    src/exchange/exchange_client.cpp
    src/exchange/websocket_client.cpp
    src/exchange/rest_client.cpp
    src/exchange/binance_client.cpp
    src/exchange/message_parser.cpp
)

set(STRATEGY_SOURCES
    src/strategy/market_maker.cpp
    src/strategy/inventory_manager.cpp
    src/strategy/signal_generator.cpp
    src/strategy/spread_calculator.cpp
)

set(ORDERBOOK_SOURCES
    src/orderbook/orderbook.cpp
    src/orderbook/price_level.cpp
    src/orderbook/order.cpp
)

set(RISK_SOURCES
    src/risk/risk_manager.cpp
    src/risk/position_manager.cpp
    src/risk/pnl_calculator.cpp
)

set(UTILS_SOURCES
    src/utils/config_parser.cpp
    src/utils/logger.cpp
    src/utils/metrics.cpp
)

set(NETWORK_SOURCES
    src/network/tcp_client.cpp
    src/network/ssl_context.cpp
)

# Main library
add_library(hft_core STATIC
    ${CORE_SOURCES}
    ${EXCHANGE_SOURCES}
    ${STRATEGY_SOURCES}
    ${ORDERBOOK_SOURCES}
    ${RISK_SOURCES}
    ${UTILS_SOURCES}
    ${NETWORK_SOURCES}
)

target_link_libraries(hft_core
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    ${Boost_LIBRARIES}
)

if(LIBURING_FOUND)
    target_compile_definitions(hft_core PRIVATE USE_IO_URING)
    target_link_libraries(hft_core ${LIBURING_LIBRARIES})
endif()

# Main executable
add_executable(market_maker src/main.cpp)
target_link_libraries(market_maker hft_core)

# Benchmark executable
add_executable(benchmark tests/benchmark.cpp)
target_link_libraries(benchmark hft_core)

# Unit tests
enable_testing()
add_executable(unit_tests
    tests/test_orderbook.cpp
    tests/test_risk.cpp
    tests/test_strategy.cpp
)
target_link_libraries(unit_tests hft_core)
add_test(NAME UnitTests COMMAND unit_tests)

# Install
install(TARGETS market_maker DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/hft)
