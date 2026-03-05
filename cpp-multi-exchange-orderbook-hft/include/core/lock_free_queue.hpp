#pragma once

/**
 * @file lock_free_queue.hpp
 * @brief Lock-free SPSC (Single Producer Single Consumer) queue implementation
 *
 * This queue is optimized for ultra-low latency scenarios in HFT systems.
 * It uses atomic operations with careful memory ordering for thread safety
 * without locks. The design is cache-line optimized to prevent false sharing.
 *
 * Key features:
 * - Lock-free SPSC design
 * - No dynamic allocation after construction
 * - Cache-line aligned to prevent false sharing
 * - Bounded capacity (power of 2 for efficient modulo)
 */

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>

#include "types.hpp"

namespace hft::core {

// Forward declaration
template<typename T, size_t Capacity>
class SPSCQueue;

/**
 * @brief Lock-free Single Producer Single Consumer Queue
 *
 * Thread-safety guarantees:
 * - push() must only be called from a single producer thread
 * - pop() must only be called from a single consumer thread
 * - push() and pop() can be called concurrently from their respective threads
 *
 * @tparam T Element type (must be trivially copyable for optimal performance)
 * @tparam Capacity Queue capacity (must be power of 2)
 */
template<typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    using value_type = T;
    using size_type = size_t;

    static constexpr size_t capacity() noexcept { return Capacity; }
    static constexpr size_t MASK = Capacity - 1;

    /**
     * @brief Constructs an empty queue
     */
    SPSCQueue() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        cached_head_ = 0;
        cached_tail_ = 0;
    }

    // Non-copyable and non-movable
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;

    ~SPSCQueue() = default;

    /**
     * @brief Attempts to push an element to the queue (producer only)
     *
     * @param value Element to push
     * @return true if element was pushed, false if queue is full
     *
     * Thread safety: Must only be called from producer thread
     */
    [[nodiscard]] bool try_push(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & MASK;

        // Check if queue is full using cached head for optimization
        if (next_tail == cached_head_) {
            // Cache miss - reload actual head
            cached_head_ = head_.load(std::memory_order_acquire);
            if (next_tail == cached_head_) {
                return false;  // Queue is actually full
            }
        }

        // Store the element
        new (&buffer_[tail]) T(value);

        // Publish the element (release ensures element is visible before tail update)
        tail_.store(next_tail, std::memory_order_release);

        return true;
    }

    /**
     * @brief Attempts to push an element using move semantics (producer only)
     *
     * @param value Element to push (will be moved)
     * @return true if element was pushed, false if queue is full
     */
    [[nodiscard]] bool try_push(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & MASK;

        if (next_tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (next_tail == cached_head_) {
                return false;
            }
        }

        new (&buffer_[tail]) T(std::move(value));
        tail_.store(next_tail, std::memory_order_release);

        return true;
    }

    /**
     * @brief Constructs an element in-place at the end of the queue
     *
     * @tparam Args Constructor argument types
     * @param args Arguments forwarded to element constructor
     * @return true if element was emplaced, false if queue is full
     */
    template<typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & MASK;

        if (next_tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (next_tail == cached_head_) {
                return false;
            }
        }

        new (&buffer_[tail]) T(std::forward<Args>(args)...);
        tail_.store(next_tail, std::memory_order_release);

        return true;
    }

    /**
     * @brief Attempts to pop an element from the queue (consumer only)
     *
     * @param value Reference to store the popped element
     * @return true if an element was popped, false if queue is empty
     *
     * Thread safety: Must only be called from consumer thread
     */
    [[nodiscard]] bool try_pop(T& value) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const size_t head = head_.load(std::memory_order_relaxed);

        // Check if queue is empty using cached tail for optimization
        if (head == cached_tail_) {
            // Cache miss - reload actual tail
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) {
                return false;  // Queue is actually empty
            }
        }

        // Read the element (acquire ensures we see the element after reading tail)
        value = std::move(*reinterpret_cast<T*>(&buffer_[head]));

        // Destroy the element in buffer
        reinterpret_cast<T*>(&buffer_[head])->~T();

        // Publish consumption (release ensures destructor completes before head update)
        head_.store((head + 1) & MASK, std::memory_order_release);

        return true;
    }

    /**
     * @brief Attempts to pop an element, returning optional
     *
     * @return std::optional containing the element, or std::nullopt if empty
     */
    [[nodiscard]] std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
        T value;
        if (try_pop(value)) {
            return std::optional<T>(std::move(value));
        }
        return std::nullopt;
    }

    /**
     * @brief Peeks at the front element without removing it
     *
     * @return Pointer to front element, or nullptr if empty
     *
     * Note: The returned pointer is only valid until the next pop operation
     */
    [[nodiscard]] const T* front() const noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);

        if (head == tail) {
            return nullptr;
        }

        return reinterpret_cast<const T*>(&buffer_[head]);
    }

    /**
     * @brief Returns approximate size of the queue
     *
     * Note: This is only accurate if called from a single thread
     * or when no concurrent push/pop operations are occurring.
     */
    [[nodiscard]] size_t size_approx() const noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return (tail - head) & MASK;
    }

    /**
     * @brief Checks if the queue is empty
     *
     * Note: This is only a snapshot; the queue state may change immediately.
     */
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Checks if the queue is full
     *
     * Note: This is only a snapshot; the queue state may change immediately.
     */
    [[nodiscard]] bool full() const noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_relaxed);
        return ((tail + 1) & MASK) == head;
    }

    /**
     * @brief Resets the queue to empty state
     *
     * Warning: This is NOT thread-safe. Only call when no concurrent access.
     */
    void reset() noexcept {
        // Destroy any remaining elements
        size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);

        while (head != tail) {
            reinterpret_cast<T*>(&buffer_[head])->~T();
            head = (head + 1) & MASK;
        }

        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        cached_head_ = 0;
        cached_tail_ = 0;
    }

private:
    // Element storage using aligned storage for proper alignment
    using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;

    // Buffer for elements
    alignas(CACHE_LINE_SIZE) Storage buffer_[Capacity];

    // Producer variables (cache line isolated)
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;
    size_t cached_head_;  // Producer's cached view of head

    // Consumer variables (cache line isolated)
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    size_t cached_tail_;  // Consumer's cached view of tail

    // Padding to prevent false sharing with adjacent objects
    char padding_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>) - sizeof(size_t)];
};

/**
 * @brief MPMC (Multiple Producer Multiple Consumer) bounded queue
 *
 * This is a more heavyweight queue that supports multiple producers
 * and multiple consumers. Use SPSCQueue when possible for better performance.
 *
 * @tparam T Element type
 * @tparam Capacity Queue capacity (must be power of 2)
 */
template<typename T, size_t Capacity>
class MPMCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    static constexpr size_t MASK = Capacity - 1;

private:
    // Element storage using aligned storage for proper alignment
    using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;

    struct Cell {
        std::atomic<size_t> sequence;
        Storage storage;
    };

public:
    MPMCQueue() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;

    ~MPMCQueue() {
        // Destroy remaining elements
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & MASK];
            size_t seq = cell.sequence.load(std::memory_order_relaxed);
            if (static_cast<intptr_t>(seq - (pos + 1)) < 0) {
                break;
            }
            reinterpret_cast<T*>(&cell.storage)->~T();
            ++pos;
        }
    }

    /**
     * @brief Attempts to push an element (any thread)
     */
    [[nodiscard]] bool try_push(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & MASK];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Queue is full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        new (&cell->storage) T(value);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Attempts to pop an element (any thread)
     */
    [[nodiscard]] bool try_pop(T& value) noexcept(std::is_nothrow_move_assignable_v<T>) {
        Cell* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & MASK];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Queue is empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        value = std::move(*reinterpret_cast<T*>(&cell->storage));
        reinterpret_cast<T*>(&cell->storage)->~T();
        cell->sequence.store(pos + MASK + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        const Cell& cell = buffer_[pos & MASK];
        size_t seq = cell.sequence.load(std::memory_order_relaxed);
        return static_cast<intptr_t>(seq - (pos + 1)) < 0;
    }

private:
    alignas(CACHE_LINE_SIZE) Cell buffer_[Capacity];
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> enqueue_pos_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> dequeue_pos_;
};

}  // namespace hft::core
