================================================================================
LIBRARIES FOR HFT SYSTEMS - README
================================================================================

OVERVIEW: Complete documentation for C++ libraries used in High-Frequency
Trading systems. Covers standard library, external dependencies, networking,
serialization, data structures, and utility libraries.

FILE LISTING:
01_standard_library.txt    - STL containers, algorithms, threading
02_external_required.txt   - Boost, spdlog, Google Test, simdjson
03_external_optional.txt   - jemalloc, mimalloc, Abseil, fmt
04_networking_libraries.txt- Boost.Beast, libcurl, WebSocket++, ZeroMQ
05_serialization.txt       - JSON, Protobuf, FlatBuffers, Cap'n Proto
06_data_structures.txt     - Lock-free queues, concurrent maps
07_utility_libraries.txt   - date, CLI11, fmt
08_creating_internal_libs.txt - Building reusable HFT components

QUICK INSTALL:
sudo apt-get install -y libboost-all-dev libspdlog-dev googletest libjemalloc-dev

CMake Integration:
find_package(Boost REQUIRED COMPONENTS system thread)
target_link_libraries(trading_engine PRIVATE Boost::system Boost::thread)

Last Updated: 2025-11-25 | Version: 1.0.0
