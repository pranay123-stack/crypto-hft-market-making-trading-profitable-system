#pragma once

#include <atomic>
#include <array>
#include <optional>
#include <cstddef>

namespace hft {

// ============================================================================
// Lock-Free SPSC Queue (Single Producer Single Consumer)
// Optimized for HFT with cache-line padding to prevent false sharing
// ============================================================================

template<typename T, size_t Capacity>
class LockFreeQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    LockFreeQueue() : head_(0), tail_(0) {
        for (auto& slot : buffer_) {
            slot.sequence.store(0, std::memory_order_relaxed);
        }
    }

    // Non-copyable, non-movable
    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    // Producer: Try to push an element
    template<typename U>
    [[nodiscard]] bool try_push(U&& item) noexcept {
        const size_t pos = tail_.load(std::memory_order_relaxed);
        Slot& slot = buffer_[pos & MASK];

        const size_t seq = slot.sequence.load(std::memory_order_acquire);

        if (seq != pos) {
            return false;  // Queue is full
        }

        slot.data = std::forward<U>(item);
        slot.sequence.store(pos + 1, std::memory_order_release);
        tail_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    // Consumer: Try to pop an element
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const size_t pos = head_.load(std::memory_order_relaxed);
        Slot& slot = buffer_[pos & MASK];

        const size_t seq = slot.sequence.load(std::memory_order_acquire);

        if (seq != pos + 1) {
            return std::nullopt;  // Queue is empty
        }

        T item = std::move(slot.data);
        slot.sequence.store(pos + Capacity, std::memory_order_release);
        head_.store(pos + 1, std::memory_order_relaxed);
        return item;
    }

    // Check if empty (approximate)
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    // Get approximate size
    [[nodiscard]] size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return tail - head;
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr size_t MASK = Capacity - 1;
    static constexpr size_t CACHE_LINE_SIZE = 64;

    struct alignas(CACHE_LINE_SIZE) Slot {
        std::atomic<size_t> sequence;
        T data;
    };

    // Padding to prevent false sharing
    alignas(CACHE_LINE_SIZE) std::array<Slot, Capacity> buffer_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;
};

// ============================================================================
// Lock-Free MPMC Queue (Multiple Producer Multiple Consumer)
// ============================================================================

template<typename T, size_t Capacity>
class MPMCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

public:
    MPMCQueue() {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;

    template<typename U>
    [[nodiscard]] bool try_push(U&& item) noexcept {
        Cell* cell;
        size_t pos = tail_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & MASK];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Full
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }

        cell->data = std::forward<U>(item);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() noexcept {
        Cell* cell;
        size_t pos = head_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & MASK];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return std::nullopt;  // Empty
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }

        T item = std::move(cell->data);
        cell->sequence.store(pos + MASK + 1, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t size() const noexcept {
        return tail_.load(std::memory_order_relaxed) -
               head_.load(std::memory_order_relaxed);
    }

private:
    static constexpr size_t MASK = Capacity - 1;
    static constexpr size_t CACHE_LINE_SIZE = 64;

    struct alignas(CACHE_LINE_SIZE) Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    alignas(CACHE_LINE_SIZE) std::array<Cell, Capacity> buffer_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;
};

} // namespace hft
