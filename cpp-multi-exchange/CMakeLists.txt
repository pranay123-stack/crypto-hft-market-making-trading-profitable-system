cmake_minimum_required(VERSION 3.20)
project(crypto_hft_multi_exchange VERSION 1.0.0 LANGUAGES CXX)

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
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math -funroll-loops")

# Find required packages
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(Boost 1.75 REQUIRED COMPONENTS system)

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
    src/core/timestamp.cpp
)

set(EXCHANGE_SOURCES
    src/exchange/exchange_manager.cpp
    src/exchange/binance_client.cpp
    src/exchange/bybit_client.cpp
    src/exchange/okx_client.cpp
    src/exchange/websocket_client.cpp
    src/exchange/rest_client.cpp
)

set(STRATEGY_SOURCES
    src/strategy/market_maker.cpp
    src/strategy/cross_exchange_mm.cpp
    src/strategy/inventory_manager.cpp
)

set(ARBITRAGE_SOURCES
    src/arbitrage/arbitrage_detector.cpp
    src/arbitrage/cross_exchange_arb.cpp
    src/arbitrage/triangular_arb.cpp
)

set(ORDERBOOK_SOURCES
    src/orderbook/orderbook.cpp
    src/orderbook/consolidated_book.cpp
)

set(RISK_SOURCES
    src/risk/risk_manager.cpp
    src/risk/position_manager.cpp
    src/risk/cross_exchange_risk.cpp
)

set(UTILS_SOURCES
    src/utils/config_parser.cpp
    src/utils/logger.cpp
    src/utils/metrics.cpp
)

# Main library
add_library(hft_multi_core STATIC
    ${CORE_SOURCES}
    ${EXCHANGE_SOURCES}
    ${STRATEGY_SOURCES}
    ${ARBITRAGE_SOURCES}
    ${ORDERBOOK_SOURCES}
    ${RISK_SOURCES}
    ${UTILS_SOURCES}
)

target_link_libraries(hft_multi_core
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    ${Boost_LIBRARIES}
)

# Main executable
add_executable(multi_exchange_mm src/main.cpp)
target_link_libraries(multi_exchange_mm hft_multi_core)

# Arbitrage executable
add_executable(arbitrage_bot src/arbitrage_main.cpp)
target_link_libraries(arbitrage_bot hft_multi_core)

# Tests
enable_testing()
add_executable(unit_tests
    tests/test_orderbook.cpp
    tests/test_arbitrage.cpp
    tests/test_consolidated_book.cpp
)
target_link_libraries(unit_tests hft_multi_core)
add_test(NAME UnitTests COMMAND unit_tests)

# Install
install(TARGETS multi_exchange_mm arbitrage_bot DESTINATION bin)
