/**
 * @file memory_pool.cpp
 * @brief Implementation of memory pool utilities and explicit instantiations
 *
 * Most memory pool functionality is header-only (templates).
 * This file contains explicit instantiations for common configurations
 * and any non-template utilities.
 */

#include "core/memory_pool.hpp"
#include "core/types.hpp"

namespace hft::core {

// Forward declare Order structure for pool instantiation
struct Order {
    OrderId order_id;
    OrderId client_order_id;
    Price price;
    Price stop_price;
    Quantity quantity;
    Quantity filled_quantity;
    Quantity remaining_quantity;
    Side side;
    OrderType type;
    OrderStatus status;
    Exchange exchange;
    Symbol symbol;
    Timestamp create_time;
    Timestamp update_time;
    uint64_t exchange_order_id;
    char padding[8];  // Padding for cache line alignment
};

static_assert(sizeof(Order) <= 256, "Order structure too large");

// Market data tick structure
struct MarketDataTick {
    Price bid_price;
    Price ask_price;
    Quantity bid_quantity;
    Quantity ask_quantity;
    Price last_price;
    Quantity last_quantity;
    Timestamp exchange_time;
    Timestamp local_time;
    Exchange exchange;
    Symbol symbol;
    uint32_t sequence;
    uint8_t flags;
};

static_assert(sizeof(MarketDataTick) <= 128, "MarketDataTick structure too large");

// Trade structure
struct Trade {
    TradeId trade_id;
    OrderId order_id;
    Price price;
    Quantity quantity;
    Side side;
    Exchange exchange;
    Symbol symbol;
    Timestamp timestamp;
    uint64_t fee;  // In fixed-point
    char fee_currency[8];
};

static_assert(sizeof(Trade) <= 128, "Trade structure too large");

// Explicit template instantiations for common pool configurations

// Fixed pools for various block sizes
template class FixedPool<64, 4096>;
template class FixedPool<64, 16384>;
template class FixedPool<64, 65536>;
template class FixedPool<64, 262144>;

template class FixedPool<128, 4096>;
template class FixedPool<128, 16384>;
template class FixedPool<128, 65536>;

template class FixedPool<256, 4096>;
template class FixedPool<256, 16384>;
template class FixedPool<256, 65536>;

// Object pools for specific types
template class ObjectPool<Order, 4096>;
template class ObjectPool<Order, 16384>;
template class ObjectPool<Order, 65536>;

template class ObjectPool<MarketDataTick, 16384>;
template class ObjectPool<MarketDataTick, 65536>;
template class ObjectPool<MarketDataTick, 262144>;

template class ObjectPool<Trade, 4096>;
template class ObjectPool<Trade, 16384>;

// Pool allocators for STL containers
template class PoolAllocator<Order, 4096>;
template class PoolAllocator<MarketDataTick, 16384>;
template class PoolAllocator<Trade, 4096>;

// Global pool instances for system-wide use
namespace {

// These pools are lazily initialized on first use
FixedPool<64, 262144>& get_small_block_pool() {
    static FixedPool<64, 262144> pool;
    return pool;
}

FixedPool<128, 131072>& get_medium_block_pool() {
    static FixedPool<128, 131072> pool;
    return pool;
}

FixedPool<256, 65536>& get_large_block_pool() {
    static FixedPool<256, 65536> pool;
    return pool;
}

}  // anonymous namespace

// Public API for global pools
void* allocate_small_block() noexcept {
    return get_small_block_pool().allocate();
}

void deallocate_small_block(void* ptr) noexcept {
    get_small_block_pool().deallocate(ptr);
}

void* allocate_medium_block() noexcept {
    return get_medium_block_pool().allocate();
}

void deallocate_medium_block(void* ptr) noexcept {
    get_medium_block_pool().deallocate(ptr);
}

void* allocate_large_block() noexcept {
    return get_large_block_pool().allocate();
}

void deallocate_large_block(void* ptr) noexcept {
    get_large_block_pool().deallocate(ptr);
}

// Pool statistics
struct PoolStats {
    size_t small_allocated;
    size_t small_available;
    size_t medium_allocated;
    size_t medium_available;
    size_t large_allocated;
    size_t large_available;
};

PoolStats get_global_pool_stats() noexcept {
    return PoolStats{
        .small_allocated = get_small_block_pool().allocated_count(),
        .small_available = get_small_block_pool().available_count(),
        .medium_allocated = get_medium_block_pool().allocated_count(),
        .medium_available = get_medium_block_pool().available_count(),
        .large_allocated = get_large_block_pool().allocated_count(),
        .large_available = get_large_block_pool().available_count()
    };
}

}  // namespace hft::core
