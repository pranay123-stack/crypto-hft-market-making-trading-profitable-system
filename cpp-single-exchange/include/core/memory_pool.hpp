#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <new>

namespace hft {

// ============================================================================
// Fixed-Size Memory Pool - Zero allocation latency after initialization
// ============================================================================

template<typename T, size_t PoolSize>
class MemoryPool {
public:
    MemoryPool() {
        // Initialize free list
        for (size_t i = 0; i < PoolSize - 1; ++i) {
            blocks_[i].next = &blocks_[i + 1];
        }
        blocks_[PoolSize - 1].next = nullptr;
        free_list_.store(&blocks_[0], std::memory_order_relaxed);
        allocated_.store(0, std::memory_order_relaxed);
    }

    ~MemoryPool() = default;

    // Non-copyable
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // Allocate a block - O(1) lock-free
    [[nodiscard]] T* allocate() noexcept {
        Block* block = pop_free_list();
        if (!block) {
            return nullptr;  // Pool exhausted
        }
        allocated_.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<T*>(&block->data);
    }

    // Deallocate a block - O(1) lock-free
    void deallocate(T* ptr) noexcept {
        if (!ptr) return;

        Block* block = reinterpret_cast<Block*>(
            reinterpret_cast<char*>(ptr) - offsetof(Block, data)
        );
        push_free_list(block);
        allocated_.fetch_sub(1, std::memory_order_relaxed);
    }

    // Construct in-place
    template<typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        T* ptr = allocate();
        if (ptr) {
            new (ptr) T(std::forward<Args>(args)...);
        }
        return ptr;
    }

    // Destroy and deallocate
    void destroy(T* ptr) noexcept {
        if (ptr) {
            ptr->~T();
            deallocate(ptr);
        }
    }

    [[nodiscard]] size_t allocated_count() const noexcept {
        return allocated_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t available_count() const noexcept {
        return PoolSize - allocated_count();
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return PoolSize;
    }

private:
    struct Block {
        Block* next;
        alignas(T) std::byte data[sizeof(T)];
    };

    Block* pop_free_list() noexcept {
        Block* old_head = free_list_.load(std::memory_order_acquire);
        while (old_head) {
            Block* new_head = old_head->next;
            if (free_list_.compare_exchange_weak(old_head, new_head,
                    std::memory_order_release, std::memory_order_acquire)) {
                return old_head;
            }
        }
        return nullptr;
    }

    void push_free_list(Block* block) noexcept {
        Block* old_head = free_list_.load(std::memory_order_relaxed);
        do {
            block->next = old_head;
        } while (!free_list_.compare_exchange_weak(old_head, block,
                    std::memory_order_release, std::memory_order_relaxed));
    }

    std::array<Block, PoolSize> blocks_;
    std::atomic<Block*> free_list_;
    std::atomic<size_t> allocated_;
};

// ============================================================================
// Object Pool with Smart Pointer Support
// ============================================================================

template<typename T, size_t PoolSize>
class ObjectPool {
public:
    class Deleter {
    public:
        explicit Deleter(ObjectPool* pool) : pool_(pool) {}

        void operator()(T* ptr) const {
            if (pool_ && ptr) {
                pool_->release(ptr);
            }
        }

    private:
        ObjectPool* pool_;
    };

    using UniquePtr = std::unique_ptr<T, Deleter>;

    ObjectPool() = default;

    template<typename... Args>
    [[nodiscard]] UniquePtr acquire(Args&&... args) {
        T* ptr = pool_.construct(std::forward<Args>(args)...);
        return UniquePtr(ptr, Deleter(this));
    }

    void release(T* ptr) {
        pool_.destroy(ptr);
    }

    [[nodiscard]] size_t allocated_count() const noexcept {
        return pool_.allocated_count();
    }

    [[nodiscard]] size_t available_count() const noexcept {
        return pool_.available_count();
    }

private:
    MemoryPool<T, PoolSize> pool_;
};

// ============================================================================
// Ring Buffer Allocator - For sequential allocation patterns
// ============================================================================

template<size_t BufferSize>
class RingBufferAllocator {
public:
    RingBufferAllocator() : head_(0), tail_(0) {}

    template<typename T>
    [[nodiscard]] T* allocate(size_t count = 1) noexcept {
        const size_t size = sizeof(T) * count;
        const size_t alignment = alignof(T);

        size_t current = head_.load(std::memory_order_relaxed);
        size_t aligned = (current + alignment - 1) & ~(alignment - 1);
        size_t new_head = aligned + size;

        if (new_head > BufferSize) {
            // Wrap around
            aligned = 0;
            new_head = size;
            if (new_head > tail_.load(std::memory_order_acquire)) {
                return nullptr;  // Buffer full
            }
        }

        head_.store(new_head, std::memory_order_release);
        return reinterpret_cast<T*>(&buffer_[aligned]);
    }

    void reset() noexcept {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }

    void advance_tail(size_t pos) noexcept {
        tail_.store(pos, std::memory_order_release);
    }

private:
    alignas(64) std::array<std::byte, BufferSize> buffer_;
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};

} // namespace hft
