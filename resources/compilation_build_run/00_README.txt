================================================================================
BUILD SYSTEM DOCUMENTATION - HIGH-FREQUENCY TRADING SYSTEM
================================================================================

OVERVIEW
--------
Comprehensive guide to building, compiling, and running the HFT C++ system.
Covers CMake build configuration, compiler optimization, linking strategies,
and deployment workflows for development, staging, and production environments.

QUICK START
-----------
For experienced developers who want to get started immediately:

# Clone or navigate to source
cd /path/to/hft_system

# Configure build (Release mode with optimizations)
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++-13 \
    -DCMAKE_CXX_FLAGS="-O3 -march=native -mtune=native -flto" \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --parallel $(nproc)

# Run
./build/bin/hft_trading_engine --config config/production.yaml

BUILD SYSTEM ARCHITECTURE
--------------------------
The HFT system uses modern CMake (3.28+) with the following structure:

HFT_System/
├── CMakeLists.txt                 # Root build configuration
├── cmake/                         # CMake modules and utilities
│   ├── CompilerFlags.cmake
│   ├── Dependencies.cmake
│   └── Installation.cmake
├── src/                           # Source code
│   ├── core/                      # Core trading engine
│   ├── exchange/                  # Exchange connectors
│   ├── strategy/                  # Trading strategies
│   └── utils/                     # Utilities
├── include/                       # Public headers
├── tests/                         # Unit tests
├── benchmarks/                    # Performance benchmarks
└── build/                         # Build output (generated)
    ├── bin/                       # Executables
    ├── lib/                       # Libraries
    └── tests/                     # Test executables

DOCUMENTATION FILES
-------------------
This directory contains 8 comprehensive guides:

01_cmake_basics.txt
   - CMake fundamentals
   - Project structure
   - Target definitions
   - Basic configuration
   Estimated reading: 20 minutes

02_cmake_advanced.txt
   - Cross-compilation
   - Toolchain files
   - Package management
   - Generator expressions
   Estimated reading: 30 minutes

03_compilers.txt
   - GCC vs Clang vs MSVC
   - Optimization levels
   - Compiler flags for HFT
   - Static analysis
   Estimated reading: 25 minutes

04_build_types.txt
   - Debug vs Release builds
   - RelWithDebInfo configuration
   - Profile-guided optimization
   - Link-time optimization
   Estimated reading: 20 minutes

05_linking.txt
   - Static vs dynamic linking
   - Symbol visibility
   - Library dependencies
   - RPATH configuration
   Estimated reading: 25 minutes

06_running_system.txt
   - Command-line arguments
   - Configuration files
   - Environment variables
   - Process management
   Estimated reading: 30 minutes

07_continuous_build.txt
   - CI/CD pipelines
   - Automated testing
   - Docker builds
   - Deployment automation
   Estimated reading: 35 minutes

TYPICAL BUILD WORKFLOWS
========================

Development Build (Debug):
-------------------------
# Full debug info, no optimizations, assertions enabled
cmake -B build/debug -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build/debug
./build/debug/bin/hft_trading_engine

Staging Build (Release with Debug Info):
----------------------------------------
# Optimized but with debug symbols for profiling
cmake -B build/staging -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-O2 -g -march=x86-64-v3"

cmake --build build/staging
./build/staging/bin/hft_trading_engine

Production Build (Maximum Performance):
---------------------------------------
# Aggressive optimizations, no debug info, minimal size
cmake -B build/production -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O3 -march=native -mtune=native -flto -DNDEBUG" \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON

cmake --build build/production
strip build/production/bin/hft_trading_engine  # Remove symbols
./build/production/bin/hft_trading_engine

Testing Build:
-------------
# Enable all tests and coverage
cmake -B build/test -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_TESTING=ON \
    -DENABLE_COVERAGE=ON

cmake --build build/test
ctest --test-dir build/test --output-on-failure

Benchmarking Build:
------------------
# Optimized for benchmarking with profiling support
cmake -B build/benchmark -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_BENCHMARKS=ON

cmake --build build/benchmark
./build/benchmark/bin/benchmark_orderbook

COMPILER SELECTION
==================

GCC (Recommended for Production):
---------------------------------
export CC=gcc-13
export CXX=g++-13
cmake -B build -G Ninja

Clang (Better Error Messages):
------------------------------
export CC=clang-17
export CXX=clang++-17
cmake -B build -G Ninja

Comparison:
----------
Feature                 GCC 13          Clang 17
------------------------------------------------------
Optimization Quality    Excellent       Very Good
Compile Speed          Good            Excellent
Error Messages         Good            Excellent
Link Time Optimization Excellent       Good
Recommended For        Production      Development

BUILD SYSTEM FEATURES
=====================

Parallel Builds:
---------------
# Ninja (automatically parallel)
cmake --build build

# Make (specify jobs)
cmake --build build -- -j $(nproc)

# Explicit parallel level
cmake --build build --parallel 16

Incremental Builds:
------------------
# Only rebuild changed files
cmake --build build

# Force full rebuild
cmake --build build --clean-first

# Specific target
cmake --build build --target hft_trading_engine

Build Caching:
-------------
# ccache for faster rebuilds
export CMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake -B build -G Ninja

# Check cache stats
ccache -s

Installation:
------------
# Install to /usr/local
sudo cmake --install build

# Install to custom prefix
cmake --install build --prefix /opt/hft

# Component installation
cmake --install build --component runtime

PERFORMANCE OPTIMIZATION LEVELS
================================

Level 1: Development (-O0)
-------------------------
- No optimizations
- Fast compilation
- Full debugging capability
- Latency: ~1000us typical

Level 2: Debug Optimized (-Og)
------------------------------
- Basic optimizations
- Maintains debuggability
- Good performance for testing
- Latency: ~500us typical

Level 3: Release (-O2)
---------------------
- Standard optimizations
- Good balance speed/size
- Staging environment
- Latency: ~200us typical

Level 4: Performance (-O3)
-------------------------
- Aggressive optimizations
- Larger binary size
- Production baseline
- Latency: ~100us typical

Level 5: Maximum (-O3 -march=native -flto)
------------------------------------------
- CPU-specific optimizations
- Link-time optimization
- Production target
- Latency: ~50us typical

Level 6: Profile-Guided (-fprofile-generate/use)
------------------------------------------------
- Data-driven optimizations
- Best possible performance
- Requires profiling run
- Latency: ~25us typical

TROUBLESHOOTING BUILDS
======================

Common Build Errors:
-------------------
1. "No CMAKE_CXX_COMPILER could be found"
   → Install g++ or clang++
   → Set CC and CXX environment variables

2. "Could NOT find Boost"
   → Install Boost or use vcpkg toolchain
   → Set BOOST_ROOT variable

3. "undefined reference to..."
   → Missing library in target_link_libraries
   → Check library installation

4. Build very slow
   → Use Ninja instead of Make
   → Enable ccache
   → Reduce parallel jobs if memory limited

5. Link errors
   → Check library order
   → Verify all dependencies installed
   → Check for ABI incompatibilities

Verbose Build Output:
--------------------
# CMake verbose configuration
cmake -B build --debug-output

# Verbose compilation
cmake --build build --verbose

# See actual compiler commands
cmake -B build -DCMAKE_VERBOSE_MAKEFILE=ON

Clean Build:
-----------
# Remove build directory
rm -rf build/

# Clean within build system
cmake --build build --target clean

# Clean CMake cache
rm -rf build/CMakeCache.txt build/CMakeFiles/

VERIFICATION
============

Build Verification Checklist:
-----------------------------
[  ] CMake configuration succeeds
[  ] All targets compile without warnings (-Wall -Wextra)
[  ] All unit tests pass
[  ] Benchmarks show expected performance
[  ] Binary size is reasonable
[  ] Debug symbols present (if Debug/RelWithDebInfo)
[  ] No undefined symbols (ldd output clean)
[  ] Correct dependencies linked (ldd shows expected libraries)
[  ] Executable runs without errors
[  ] Configuration files load correctly
[  ] Exchange connectivity works
[  ] Latency targets met

Verification Commands:
---------------------
# Check binary type
file build/bin/hft_trading_engine

# Check linked libraries
ldd build/bin/hft_trading_engine

# Check symbols
nm -C build/bin/hft_trading_engine | grep -i order

# Check size
size build/bin/hft_trading_engine

# Check optimization level (rough indicator)
objdump -d build/bin/hft_trading_engine | head -100

# Test run
./build/bin/hft_trading_engine --version
./build/bin/hft_trading_engine --test-connection

PERFORMANCE TARGETS
===================

Build Time Targets:
------------------
Clean Build:        < 5 minutes (with Ninja, ccache disabled)
Incremental Build:  < 10 seconds (typical change)
Link Time:          < 30 seconds (production build)

Binary Size Targets:
-------------------
Debug Build:        50-100 MB
Release Build:      10-20 MB
Stripped Release:   5-10 MB

Runtime Performance Targets:
---------------------------
Order Latency:      < 100 microseconds (P99)
Market Data:        < 50 microseconds (P99)
Risk Check:         < 20 microseconds (P99)
Memory Usage:       < 1 GB (steady state)
CPU Usage:          < 50% (average)

NEXT STEPS
==========

After reading this overview:
1. Read 01_cmake_basics.txt for CMake fundamentals
2. Review 03_compilers.txt for optimization strategies
3. Study 04_build_types.txt for build configuration
4. Implement 06_running_system.txt for deployment
5. Set up 07_continuous_build.txt for automation

For Immediate Development:
-------------------------
1. Create build directory
2. Configure with CMake
3. Build with Ninja
4. Run unit tests
5. Deploy to dev environment

For Production Deployment:
-------------------------
1. Use production build configuration
2. Enable link-time optimization
3. Profile-guided optimization (if time permits)
4. Strip debug symbols
5. Verify performance benchmarks
6. Deploy with monitoring

ADDITIONAL RESOURCES
====================

Internal Documentation:
----------------------
- architecture.md: System design
- CONTRIBUTING.md: Development guidelines
- TESTING.md: Test procedures

External References:
-------------------
- CMake: https://cmake.org/cmake/help/latest/
- GCC: https://gcc.gnu.org/onlinedocs/
- Clang: https://clang.llvm.org/docs/
- Ninja: https://ninja-build.org/manual.html

================================================================================
Last Updated: 2025-11-25
Version: 1.0
Maintainer: HFT Build Team
================================================================================
