/**
 * Core Infrastructure Benchmarks
 * Measures latency and throughput of critical components
 */

#include <benchmark/benchmark.h>
#include <random>
#include <thread>

#include "core/lock_free_queue.hpp"
#include "core/memory_pool.hpp"
#include "core/timestamp.hpp"
#include "core/thread_pool.hpp"

using namespace hft::core;

// ============================================================================
// Lock-Free Queue Benchmarks
// ============================================================================

static void BM_QueuePush(benchmark::State& state) {
    LockFreeQueue<int, 65536> queue;
    int value = 42;

    for (auto _ : state) {
        queue.push(value);
        int out;
        queue.pop(out);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QueuePush);

static void BM_QueueThroughput(benchmark::State& state) {
    LockFreeQueue<int, 65536> queue;

    for (auto _ : state) {
        // Push batch
        for (int i = 0; i < 1000; ++i) {
            queue.push(i);
        }
        // Pop batch
        int value;
        for (int i = 0; i < 1000; ++i) {
            queue.pop(value);
        }
    }

    state.SetItemsProcessed(state.iterations() * 2000);
}
BENCHMARK(BM_QueueThroughput);

static void BM_QueueSPSC(benchmark::State& state) {
    LockFreeQueue<int, 65536> queue;
    std::atomic<bool> running{true};
    std::atomic<int64_t> ops{0};

    // Consumer thread
    std::thread consumer([&]() {
        int value;
        while (running) {
            if (queue.pop(value)) {
                ops++;
            }
        }
    });

    for (auto _ : state) {
        for (int i = 0; i < 1000; ++i) {
            while (!queue.push(i)) {}
        }
    }

    running = false;
    consumer.join();

    state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_QueueSPSC);

// ============================================================================
// Memory Pool Benchmarks
// ============================================================================

struct OrderStruct {
    uint64_t id;
    double price;
    double quantity;
    char symbol[16];
    uint64_t timestamp;
};

static void BM_MemoryPoolAlloc(benchmark::State& state) {
    MemoryPool<OrderStruct, 1024> pool;

    for (auto _ : state) {
        OrderStruct* ptr = pool.allocate();
        benchmark::DoNotOptimize(ptr);
        pool.deallocate(ptr);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MemoryPoolAlloc);

static void BM_StdAllocator(benchmark::State& state) {
    for (auto _ : state) {
        OrderStruct* ptr = new OrderStruct();
        benchmark::DoNotOptimize(ptr);
        delete ptr;
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StdAllocator);

static void BM_MemoryPoolBatch(benchmark::State& state) {
    MemoryPool<OrderStruct, 4096> pool;
    std::vector<OrderStruct*> ptrs;
    ptrs.reserve(100);

    for (auto _ : state) {
        // Allocate batch
        for (int i = 0; i < 100; ++i) {
            ptrs.push_back(pool.allocate());
        }
        // Deallocate batch
        for (auto* ptr : ptrs) {
            pool.deallocate(ptr);
        }
        ptrs.clear();
    }

    state.SetItemsProcessed(state.iterations() * 100);
}
BENCHMARK(BM_MemoryPoolBatch);

// ============================================================================
// Timestamp Benchmarks
// ============================================================================

static void BM_TimestampNow(benchmark::State& state) {
    for (auto _ : state) {
        Timestamp ts = Timestamp::now();
        benchmark::DoNotOptimize(ts);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TimestampNow);

static void BM_TimestampRDTSC(benchmark::State& state) {
    for (auto _ : state) {
        uint64_t tsc = Timestamp::rdtsc();
        benchmark::DoNotOptimize(tsc);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TimestampRDTSC);

static void BM_ChronoNow(benchmark::State& state) {
    for (auto _ : state) {
        auto ts = std::chrono::high_resolution_clock::now();
        benchmark::DoNotOptimize(ts);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ChronoNow);

// ============================================================================
// Thread Pool Benchmarks
// ============================================================================

static void BM_ThreadPoolSubmit(benchmark::State& state) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};

    for (auto _ : state) {
        auto future = pool.submit([&counter]() {
            counter++;
            return 0;
        });
        future.get();
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ThreadPoolSubmit);

static void BM_ThreadPoolBatchSubmit(benchmark::State& state) {
    ThreadPool pool(state.range(0));
    std::atomic<int> counter{0};
    std::vector<std::future<int>> futures;
    futures.reserve(1000);

    for (auto _ : state) {
        for (int i = 0; i < 1000; ++i) {
            futures.push_back(pool.submit([&counter]() {
                counter++;
                return 0;
            }));
        }

        for (auto& f : futures) {
            f.get();
        }
        futures.clear();
    }

    state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_ThreadPoolBatchSubmit)->Arg(2)->Arg(4)->Arg(8)->Arg(16);

// ============================================================================
// Latency Distribution Benchmarks
// ============================================================================

static void BM_EndToEndLatency(benchmark::State& state) {
    // Simulate order processing pipeline
    LockFreeQueue<OrderStruct, 1024> order_queue;
    LockFreeQueue<OrderStruct, 1024> response_queue;
    MemoryPool<OrderStruct, 512> pool;

    std::atomic<bool> running{true};

    // "Exchange" thread - processes orders
    std::thread exchange([&]() {
        OrderStruct order;
        while (running) {
            if (order_queue.pop(order)) {
                // Simulate minimal processing
                order.timestamp = Timestamp::now().nanoseconds();
                while (!response_queue.push(order)) {}
            }
        }
    });

    std::vector<int64_t> latencies;
    latencies.reserve(state.max_iterations);

    for (auto _ : state) {
        OrderStruct* order = pool.allocate();
        order->id = 1;
        order->price = 50000.0;
        order->quantity = 0.1;
        order->timestamp = Timestamp::now().nanoseconds();

        // Send order
        while (!order_queue.push(*order)) {}

        // Wait for response
        OrderStruct response;
        while (!response_queue.pop(response)) {}

        int64_t latency = Timestamp::now().nanoseconds() - order->timestamp;
        latencies.push_back(latency);

        pool.deallocate(order);
    }

    running = false;
    exchange.join();

    // Calculate latency statistics
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        state.counters["p50_ns"] = latencies[latencies.size() / 2];
        state.counters["p99_ns"] = latencies[latencies.size() * 99 / 100];
        state.counters["p999_ns"] = latencies[latencies.size() * 999 / 1000];
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EndToEndLatency)->Iterations(10000);

// ============================================================================
// Main
// ============================================================================

BENCHMARK_MAIN();
