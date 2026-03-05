#pragma once

/**
 * @file memory_pool.hpp
 * @brief High-performance memory pool for HFT applications
 *
 * This memory pool is designed for ultra-low latency allocation:
 * - Fixed-size blocks eliminate fragmentation
 * - Pre-allocated pools avoid runtime allocation
 * - Thread-local allocators minimize contention
 * - Lock-free free-list for thread-safe deallocation
 * - Cache-line alignment for optimal memory access
 */

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <cassert>

#include "types.hpp"

namespace hft::core {

/**
 * @brief Fixed-size block memory pool
 *
 * Allocates memory in fixed-size blocks from a pre-allocated buffer.
 * Ideal for objects that are frequently allocated and deallocated.
 *
 * @tparam BlockSize Size of each block in bytes
 * @tparam NumBlocks Number of blocks in the pool
 * @tparam Alignment Alignment requirement for blocks
 */
template<size_t BlockSize, size_t NumBlocks, size_t Alignment = CACHE_LINE_SIZE>
class FixedPool {
    static_assert(BlockSize >= sizeof(void*), "Block size must be at least pointer size");
    static_assert(NumBlocks > 0, "Must have at least one block");
    static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be power of 2");

public:
    static constexpr size_t BLOCK_SIZE = BlockSize;
    static constexpr size_t POOL_SIZE = NumBlocks;
    static constexpr size_t ALIGNED_BLOCK_SIZE =
        (BlockSize + Alignment - 1) & ~(Alignment - 1);

    /**
     * @brief Constructs the pool and initializes free list
     */
    FixedPool() noexcept {
        // Initialize free list by linking all blocks
        for (size_t i = 0; i < NumBlocks - 1; ++i) {
            Block* block = get_block(i);
            block->next = get_block(i + 1);
        }
        get_block(NumBlocks - 1)->next = nullptr;

        free_list_.store(get_block(0), std::memory_order_relaxed);
        allocated_count_.store(0, std::memory_order_relaxed);
    }

    // Non-copyable and non-movable
    FixedPool(const FixedPool&) = delete;
    FixedPool& operator=(const FixedPool&) = delete;
    FixedPool(FixedPool&&) = delete;
    FixedPool& operator=(FixedPool&&) = delete;

    ~FixedPool() = default;

    /**
     * @brief Allocates a block from the pool
     *
     * @return Pointer to allocated block, or nullptr if pool is exhausted
     *
     * Thread safety: Lock-free, safe to call from any thread
     */
    [[nodiscard]] void* allocate() noexcept {
        Block* block = free_list_.load(std::memory_order_acquire);

        while (block != nullptr) {
            Block* next = block->next;
            if (free_list_.compare_exchange_weak(block, next,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                allocated_count_.fetch_add(1, std::memory_order_relaxed);
                return static_cast<void*>(block);
            }
            // CAS failed, block was updated, retry with new value
        }

        return nullptr;  // Pool exhausted
    }

    /**
     * @brief Deallocates a block back to the pool
     *
     * @param ptr Pointer to block to deallocate
     *
     * Thread safety: Lock-free, safe to call from any thread
     */
    void deallocate(void* ptr) noexcept {
        if (ptr == nullptr) return;

#ifndef NDEBUG
        // Debug check: verify pointer is within pool
        assert(owns(ptr) && "Pointer does not belong to this pool");
#endif

        Block* block = static_cast<Block*>(ptr);
        Block* old_head = free_list_.load(std::memory_order_relaxed);

        do {
            block->next = old_head;
        } while (!free_list_.compare_exchange_weak(old_head, block,
                    std::memory_order_release,
                    std::memory_order_relaxed));

        allocated_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    /**
     * @brief Checks if a pointer belongs to this pool
     */
    [[nodiscard]] bool owns(void* ptr) const noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        auto start = reinterpret_cast<uintptr_t>(storage_.data());
        auto end = start + sizeof(storage_);
        return addr >= start && addr < end;
    }

    /**
     * @brief Returns number of currently allocated blocks
     */
    [[nodiscard]] size_t allocated_count() const noexcept {
        return allocated_count_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Returns number of available blocks
     */
    [[nodiscard]] size_t available_count() const noexcept {
        return NumBlocks - allocated_count_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Returns total capacity
     */
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return NumBlocks;
    }

    /**
     * @brief Checks if pool is exhausted
     */
    [[nodiscard]] bool empty() const noexcept {
        return free_list_.load(std::memory_order_relaxed) == nullptr;
    }

    /**
     * @brief Resets pool to initial state
     *
     * Warning: NOT thread-safe. Only call when no concurrent access.
     * All outstanding allocations become invalid.
     */
    void reset() noexcept {
        for (size_t i = 0; i < NumBlocks - 1; ++i) {
            Block* block = get_block(i);
            block->next = get_block(i + 1);
        }
        get_block(NumBlocks - 1)->next = nullptr;

        free_list_.store(get_block(0), std::memory_order_release);
        allocated_count_.store(0, std::memory_order_relaxed);
    }

private:
    struct Block {
        Block* next;
        // Remaining space available for user data
        char data[ALIGNED_BLOCK_SIZE - sizeof(Block*)];
    };

    static_assert(sizeof(Block) == ALIGNED_BLOCK_SIZE, "Block size mismatch");

    [[nodiscard]] Block* get_block(size_t index) noexcept {
        return reinterpret_cast<Block*>(storage_.data() + index * ALIGNED_BLOCK_SIZE);
    }

    alignas(Alignment) std::array<std::byte, ALIGNED_BLOCK_SIZE * NumBlocks> storage_;
    alignas(CACHE_LINE_SIZE) std::atomic<Block*> free_list_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> allocated_count_;
};

/**
 * @brief Thread-local memory pool wrapper
 *
 * Provides a thread-local pool for fast allocation without contention.
 * Falls back to a shared pool when local pool is exhausted.
 *
 * @tparam BlockSize Size of each block
 * @tparam LocalBlocks Blocks per thread-local pool
 * @tparam SharedBlocks Blocks in shared overflow pool
 */
template<size_t BlockSize, size_t LocalBlocks = 256, size_t SharedBlocks = 4096>
class ThreadLocalPool {
public:
    static constexpr size_t BLOCK_SIZE = BlockSize;

    /**
     * @brief Gets thread-local pool instance
     */
    static ThreadLocalPool& instance() {
        static thread_local ThreadLocalPool local_pool;
        return local_pool;
    }

    /**
     * @brief Allocates from thread-local pool, falls back to shared pool
     */
    [[nodiscard]] void* allocate() noexcept {
        void* ptr = local_pool_.allocate();
        if (ptr != nullptr) {
            return ptr;
        }
        // Local pool exhausted, try shared pool
        return shared_pool().allocate();
    }

    /**
     * @brief Deallocates to appropriate pool
     */
    void deallocate(void* ptr) noexcept {
        if (ptr == nullptr) return;

        if (local_pool_.owns(ptr)) {
            local_pool_.deallocate(ptr);
        } else {
            shared_pool().deallocate(ptr);
        }
    }

    /**
     * @brief Returns the shared overflow pool
     */
    static FixedPool<BlockSize, SharedBlocks>& shared_pool() {
        static FixedPool<BlockSize, SharedBlocks> pool;
        return pool;
    }

private:
    ThreadLocalPool() = default;
    FixedPool<BlockSize, LocalBlocks> local_pool_;
};

/**
 * @brief Type-aware object pool
 *
 * Allocates objects of a specific type with proper construction/destruction.
 *
 * @tparam T Object type to pool
 * @tparam NumObjects Number of objects in pool
 */
template<typename T, size_t NumObjects>
class ObjectPool {
    static constexpr size_t BLOCK_SIZE =
        (sizeof(T) + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);

public:
    ObjectPool() = default;

    // Non-copyable
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    /**
     * @brief Allocates and constructs an object
     *
     * @tparam Args Constructor argument types
     * @param args Arguments forwarded to constructor
     * @return Pointer to constructed object, or nullptr if pool exhausted
     */
    template<typename... Args>
    [[nodiscard]] T* allocate(Args&&... args) {
        void* ptr = pool_.allocate();
        if (ptr == nullptr) {
            return nullptr;
        }
        return new (ptr) T(std::forward<Args>(args)...);
    }

    /**
     * @brief Destroys and deallocates an object
     */
    void deallocate(T* obj) noexcept {
        if (obj == nullptr) return;
        obj->~T();
        pool_.deallocate(obj);
    }

    /**
     * @brief Number of allocated objects
     */
    [[nodiscard]] size_t allocated_count() const noexcept {
        return pool_.allocated_count();
    }

    /**
     * @brief Number of available slots
     */
    [[nodiscard]] size_t available_count() const noexcept {
        return pool_.available_count();
    }

    /**
     * @brief Total capacity
     */
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return NumObjects;
    }

private:
    FixedPool<BLOCK_SIZE, NumObjects> pool_;
};

/**
 * @brief RAII wrapper for pooled objects
 *
 * Automatically returns object to pool on destruction.
 */
template<typename T, typename Pool>
class PooledPtr {
public:
    PooledPtr() noexcept : ptr_(nullptr), pool_(nullptr) {}

    PooledPtr(T* ptr, Pool* pool) noexcept : ptr_(ptr), pool_(pool) {}

    // Move-only
    PooledPtr(PooledPtr&& other) noexcept
        : ptr_(other.ptr_), pool_(other.pool_) {
        other.ptr_ = nullptr;
        other.pool_ = nullptr;
    }

    PooledPtr& operator=(PooledPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            pool_ = other.pool_;
            other.ptr_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }

    PooledPtr(const PooledPtr&) = delete;
    PooledPtr& operator=(const PooledPtr&) = delete;

    ~PooledPtr() {
        reset();
    }

    void reset() noexcept {
        if (ptr_ != nullptr && pool_ != nullptr) {
            pool_->deallocate(ptr_);
            ptr_ = nullptr;
        }
    }

    [[nodiscard]] T* get() const noexcept { return ptr_; }
    [[nodiscard]] T* operator->() const noexcept { return ptr_; }
    [[nodiscard]] T& operator*() const noexcept { return *ptr_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T* release() noexcept {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }

private:
    T* ptr_;
    Pool* pool_;
};

/**
 * @brief STL-compatible allocator using memory pool
 */
template<typename T, size_t PoolSize = 4096>
class PoolAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = size_t;
    using difference_type = ptrdiff_t;

    template<typename U>
    struct rebind {
        using other = PoolAllocator<U, PoolSize>;
    };

    PoolAllocator() noexcept = default;

    template<typename U>
    PoolAllocator(const PoolAllocator<U, PoolSize>&) noexcept {}

    [[nodiscard]] T* allocate(size_t n) {
        if (n == 1) {
            void* ptr = get_pool().allocate();
            if (ptr) return static_cast<T*>(ptr);
        }
        // Fallback to standard allocation for multi-element requests
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }

    void deallocate(T* ptr, size_t n) noexcept {
        if (n == 1 && get_pool().owns(ptr)) {
            get_pool().deallocate(ptr);
        } else {
            ::operator delete(ptr);
        }
    }

    template<typename U>
    bool operator==(const PoolAllocator<U, PoolSize>&) const noexcept {
        return true;
    }

    template<typename U>
    bool operator!=(const PoolAllocator<U, PoolSize>&) const noexcept {
        return false;
    }

private:
    static constexpr size_t BLOCK_SIZE =
        (sizeof(T) + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);

    static FixedPool<BLOCK_SIZE, PoolSize>& get_pool() {
        static FixedPool<BLOCK_SIZE, PoolSize> pool;
        return pool;
    }
};

// Pre-defined pool types for common HFT objects
// These sizes are typical for orders and market data structures

// Order pool: ~128 bytes per order, 64K orders
using OrderPool = ObjectPool<struct Order, 65536>;

// Market data pool: ~64 bytes per update, 256K updates
constexpr size_t MARKET_DATA_BLOCK_SIZE = 64;
constexpr size_t MARKET_DATA_POOL_SIZE = 262144;
using MarketDataPool = FixedPool<MARKET_DATA_BLOCK_SIZE, MARKET_DATA_POOL_SIZE>;

}  // namespace hft::core
