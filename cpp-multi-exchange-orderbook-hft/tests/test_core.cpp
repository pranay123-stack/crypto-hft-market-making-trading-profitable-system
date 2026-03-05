/**
 * Core Infrastructure Unit Tests
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

#include "core/types.hpp"
#include "core/lock_free_queue.hpp"
#include "core/memory_pool.hpp"
#include "core/timestamp.hpp"
#include "core/thread_pool.hpp"

using namespace hft::core;

// ============================================================================
// Lock-Free Queue Tests
// ============================================================================

class LockFreeQueueTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(LockFreeQueueTest, BasicPushPop) {
    LockFreeQueue<int, 1024> queue;

    EXPECT_TRUE(queue.push(42));
    EXPECT_FALSE(queue.empty());

    int value;
    EXPECT_TRUE(queue.pop(value));
    EXPECT_EQ(value, 42);
    EXPECT_TRUE(queue.empty());
}

TEST_F(LockFreeQueueTest, FillAndDrain) {
    LockFreeQueue<int, 128> queue;

    // Fill queue
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(queue.push(i));
    }

    // Drain queue
    for (int i = 0; i < 100; ++i) {
        int value;
        EXPECT_TRUE(queue.pop(value));
        EXPECT_EQ(value, i);
    }

    EXPECT_TRUE(queue.empty());
}

TEST_F(LockFreeQueueTest, OverflowProtection) {
    LockFreeQueue<int, 8> queue;

    // Fill to capacity
    for (int i = 0; i < 7; ++i) {  // capacity - 1
        EXPECT_TRUE(queue.push(i));
    }

    // Should fail on overflow
    EXPECT_FALSE(queue.push(999));
}

TEST_F(LockFreeQueueTest, SPSCConcurrency) {
    LockFreeQueue<int, 65536> queue;
    constexpr int NUM_ITEMS = 100000;
    std::atomic<int> sum_produced{0};
    std::atomic<int> sum_consumed{0};

    // Producer thread
    std::thread producer([&]() {
        for (int i = 1; i <= NUM_ITEMS; ++i) {
            while (!queue.push(i)) {
                std::this_thread::yield();
            }
            sum_produced += i;
        }
    });

    // Consumer thread
    std::thread consumer([&]() {
        int consumed = 0;
        while (consumed < NUM_ITEMS) {
            int value;
            if (queue.pop(value)) {
                sum_consumed += value;
                ++consumed;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(sum_produced.load(), sum_consumed.load());
}

// ============================================================================
// Memory Pool Tests
// ============================================================================

class MemoryPoolTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MemoryPoolTest, BasicAllocFree) {
    MemoryPool<int, 64> pool;

    int* ptr = pool.allocate();
    EXPECT_NE(ptr, nullptr);

    *ptr = 42;
    EXPECT_EQ(*ptr, 42);

    pool.deallocate(ptr);
}

TEST_F(MemoryPoolTest, MultipleAllocations) {
    MemoryPool<int, 64> pool;
    std::vector<int*> ptrs;

    // Allocate all
    for (int i = 0; i < 60; ++i) {
        int* ptr = pool.allocate();
        EXPECT_NE(ptr, nullptr);
        *ptr = i;
        ptrs.push_back(ptr);
    }

    // Verify values
    for (int i = 0; i < 60; ++i) {
        EXPECT_EQ(*ptrs[i], i);
    }

    // Free all
    for (auto* ptr : ptrs) {
        pool.deallocate(ptr);
    }
}

TEST_F(MemoryPoolTest, ReuseAfterFree) {
    MemoryPool<int, 4> pool;

    int* ptr1 = pool.allocate();
    pool.deallocate(ptr1);

    int* ptr2 = pool.allocate();
    EXPECT_EQ(ptr1, ptr2);  // Should reuse same block
}

// ============================================================================
// Timestamp Tests
// ============================================================================

class TimestampTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TimestampTest, Monotonicity) {
    Timestamp t1 = Timestamp::now();
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    Timestamp t2 = Timestamp::now();

    EXPECT_GT(t2.nanoseconds(), t1.nanoseconds());
}

TEST_F(TimestampTest, Arithmetic) {
    Timestamp t1(1000000000);  // 1 second
    Timestamp t2(2000000000);  // 2 seconds

    auto diff = t2 - t1;
    EXPECT_EQ(diff.nanoseconds(), 1000000000);
}

TEST_F(TimestampTest, Comparison) {
    Timestamp t1(100);
    Timestamp t2(200);

    EXPECT_LT(t1, t2);
    EXPECT_GT(t2, t1);
    EXPECT_NE(t1, t2);

    Timestamp t3(100);
    EXPECT_EQ(t1, t3);
}

TEST_F(TimestampTest, Conversion) {
    Timestamp t(1000000000);  // 1 second in nanoseconds

    EXPECT_EQ(t.microseconds(), 1000000);
    EXPECT_EQ(t.milliseconds(), 1000);
    EXPECT_EQ(t.seconds(), 1);
}

// ============================================================================
// Thread Pool Tests
// ============================================================================

class ThreadPoolTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(ThreadPoolTest, BasicTask) {
    ThreadPool pool(4);

    std::atomic<int> counter{0};
    auto future = pool.submit([&counter]() {
        counter++;
        return 42;
    });

    EXPECT_EQ(future.get(), 42);
    EXPECT_EQ(counter.load(), 1);
}

TEST_F(ThreadPoolTest, MultipleTasks) {
    ThreadPool pool(4);
    constexpr int NUM_TASKS = 1000;

    std::atomic<int> counter{0};
    std::vector<std::future<int>> futures;

    for (int i = 0; i < NUM_TASKS; ++i) {
        futures.push_back(pool.submit([&counter, i]() {
            counter++;
            return i * 2;
        }));
    }

    int sum = 0;
    for (int i = 0; i < NUM_TASKS; ++i) {
        sum += futures[i].get();
    }

    EXPECT_EQ(counter.load(), NUM_TASKS);
    EXPECT_EQ(sum, NUM_TASKS * (NUM_TASKS - 1));  // Sum of 0*2 + 1*2 + ... + 999*2
}

TEST_F(ThreadPoolTest, Shutdown) {
    auto pool = std::make_unique<ThreadPool>(2);

    std::atomic<int> counter{0};
    for (int i = 0; i < 10; ++i) {
        pool->submit([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            counter++;
        });
    }

    pool->shutdown();
    pool.reset();

    // Some tasks may not have completed, but no crash
    EXPECT_GE(counter.load(), 0);
}

// ============================================================================
// Types Tests
// ============================================================================

TEST(TypesTest, PriceType) {
    using namespace hft;

    Price p1 = 1000050000;  // $10000.50 in 0.0001 precision
    Price p2 = 1000100000;  // $10001.00

    EXPECT_LT(p1, p2);
    EXPECT_EQ(p2 - p1, 50000);  // $0.50 difference
}

TEST(TypesTest, SideEnum) {
    using namespace hft;

    EXPECT_NE(Side::Buy, Side::Sell);
}

TEST(TypesTest, OrderTypeEnum) {
    using namespace hft;

    EXPECT_NE(OrderType::Market, OrderType::Limit);
    EXPECT_NE(OrderType::Limit, OrderType::StopLimit);
}

TEST(TypesTest, ExchangeEnum) {
    using namespace hft;

    std::vector<Exchange> exchanges = {
        Exchange::Binance,
        Exchange::Bybit,
        Exchange::OKX,
        Exchange::Kraken,
        Exchange::Coinbase,
        Exchange::KuCoin,
        Exchange::GateIO,
        Exchange::Bitfinex,
        Exchange::Deribit,
        Exchange::HTX
    };

    EXPECT_EQ(exchanges.size(), 10);

    // Ensure all unique
    std::set<Exchange> unique_exchanges(exchanges.begin(), exchanges.end());
    EXPECT_EQ(unique_exchanges.size(), 10);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
