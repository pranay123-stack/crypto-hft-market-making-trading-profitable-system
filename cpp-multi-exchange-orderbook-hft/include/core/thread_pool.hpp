#pragma once

/**
 * @file thread_pool.hpp
 * @brief Work-stealing thread pool with CPU affinity support
 *
 * This thread pool is designed for HFT applications:
 * - Work-stealing for load balancing
 * - CPU affinity to pin threads to cores
 * - Priority-based task scheduling
 * - Lock-free task queues
 * - Minimal latency overhead
 *
 * Usage:
 *   ThreadPool pool(4);  // 4 worker threads
 *   auto future = pool.submit(Priority::High, []{ return compute(); });
 *   auto result = future.get();
 */

#include <atomic>
#include <vector>
#include <thread>
#include <functional>
#include <future>
#include <optional>
#include <random>
#include <memory>
#include <deque>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <type_traits>

#include "types.hpp"

namespace hft::core {

/**
 * @brief Task priority levels
 */
enum class Priority : uint8_t {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3  // For time-critical operations
};

/**
 * @brief Lock-free work-stealing deque (Chase-Lev algorithm)
 *
 * Supports push/pop from one end (owner thread) and steal from other end.
 */
template<typename T>
class WorkStealingDeque {
public:
    static constexpr size_t INITIAL_CAPACITY = 1024;

    WorkStealingDeque()
        : buffer_(new CircularArray(INITIAL_CAPACITY))
        , top_(0)
        , bottom_(0) {}

    ~WorkStealingDeque() {
        delete buffer_.load(std::memory_order_relaxed);
    }

    // Non-copyable
    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

    /**
     * @brief Push task onto bottom (owner only)
     */
    void push(T item) {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_acquire);
        CircularArray* array = buffer_.load(std::memory_order_relaxed);

        if (b - t > static_cast<int64_t>(array->size()) - 1) {
            // Array is full, grow it
            array = array->grow(t, b);
            buffer_.store(array, std::memory_order_release);
        }

        array->put(b, std::move(item));
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    /**
     * @brief Pop task from bottom (owner only)
     */
    std::optional<T> pop() {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        CircularArray* array = buffer_.load(std::memory_order_relaxed);
        bottom_.store(b, std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        int64_t t = top_.load(std::memory_order_relaxed);

        if (t <= b) {
            // Non-empty
            T item = std::move(array->get(b));
            if (t == b) {
                // Last item, race with steal
                if (!top_.compare_exchange_strong(t, t + 1,
                        std::memory_order_seq_cst,
                        std::memory_order_relaxed)) {
                    // Lost race
                    bottom_.store(b + 1, std::memory_order_relaxed);
                    return std::nullopt;
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
            return item;
        } else {
            // Empty
            bottom_.store(b + 1, std::memory_order_relaxed);
            return std::nullopt;
        }
    }

    /**
     * @brief Steal task from top (other threads)
     */
    std::optional<T> steal() {
        int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t b = bottom_.load(std::memory_order_acquire);

        if (t < b) {
            // Non-empty
            CircularArray* array = buffer_.load(std::memory_order_consume);
            T item = std::move(array->get(t));
            if (!top_.compare_exchange_strong(t, t + 1,
                    std::memory_order_seq_cst,
                    std::memory_order_relaxed)) {
                // Lost race
                return std::nullopt;
            }
            return item;
        }
        return std::nullopt;
    }

    /**
     * @brief Check if empty
     */
    [[nodiscard]] bool empty() const noexcept {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        return b <= t;
    }

    /**
     * @brief Approximate size
     */
    [[nodiscard]] size_t size() const noexcept {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        return static_cast<size_t>(std::max(int64_t(0), b - t));
    }

private:
    class CircularArray {
    public:
        explicit CircularArray(size_t n) : size_(n), mask_(n - 1), items_(new T[n]) {}
        ~CircularArray() { delete[] items_; }

        [[nodiscard]] size_t size() const noexcept { return size_; }

        T& get(int64_t i) noexcept {
            return items_[i & mask_];
        }

        const T& get(int64_t i) const noexcept {
            return items_[i & mask_];
        }

        void put(int64_t i, T item) noexcept {
            items_[i & mask_] = std::move(item);
        }

        CircularArray* grow(int64_t top, int64_t bottom) {
            CircularArray* new_array = new CircularArray(size_ * 2);
            for (int64_t i = top; i < bottom; ++i) {
                new_array->put(i, std::move(items_[i & mask_]));
            }
            return new_array;
        }

    private:
        size_t size_;
        size_t mask_;
        T* items_;
    };

    std::atomic<CircularArray*> buffer_;
    alignas(CACHE_LINE_SIZE) std::atomic<int64_t> top_;
    alignas(CACHE_LINE_SIZE) std::atomic<int64_t> bottom_;
};

/**
 * @brief Task wrapper for type-erased execution
 */
class Task {
public:
    Task() = default;

    template<typename F>
    explicit Task(F&& f) : impl_(std::make_unique<Model<F>>(std::forward<F>(f))) {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    void operator()() {
        if (impl_) {
            impl_->invoke();
        }
    }

    explicit operator bool() const noexcept {
        return impl_ != nullptr;
    }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void invoke() = 0;
    };

    template<typename F>
    struct Model : Concept {
        explicit Model(F&& f) : func_(std::forward<F>(f)) {}
        void invoke() override { func_(); }
        F func_;
    };

    std::unique_ptr<Concept> impl_;
};

/**
 * @brief Prioritized task wrapper
 */
struct PrioritizedTask {
    Task task;
    Priority priority;
    uint64_t sequence;  // For FIFO ordering within same priority

    PrioritizedTask() = default;
    PrioritizedTask(Task t, Priority p, uint64_t seq)
        : task(std::move(t)), priority(p), sequence(seq) {}

    // Higher priority comes first, then lower sequence (earlier submission)
    bool operator<(const PrioritizedTask& other) const noexcept {
        if (priority != other.priority) {
            return static_cast<uint8_t>(priority) < static_cast<uint8_t>(other.priority);
        }
        return sequence > other.sequence;  // Lower sequence = higher priority
    }
};

/**
 * @brief Work-stealing thread pool with priority scheduling
 */
class ThreadPool {
public:
    /**
     * @brief Constructs thread pool with specified number of workers
     *
     * @param num_threads Number of worker threads (0 = hardware concurrency)
     * @param pin_to_cpu Whether to pin threads to CPU cores
     */
    explicit ThreadPool(size_t num_threads = 0, bool pin_to_cpu = false);

    /**
     * @brief Destructor - waits for all tasks to complete
     */
    ~ThreadPool();

    // Non-copyable and non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * @brief Submits a task for execution
     *
     * @tparam F Callable type
     * @tparam Args Argument types
     * @param priority Task priority
     * @param f Callable to execute
     * @param args Arguments to pass to callable
     * @return Future for the result
     */
    template<typename F, typename... Args>
    auto submit(Priority priority, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {

        using ReturnType = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<ReturnType> future = task->get_future();

        uint64_t seq = task_sequence_.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(global_mutex_);
            global_queue_.emplace(
                Task([task]() { (*task)(); }),
                priority,
                seq
            );
        }

        cv_.notify_one();
        return future;
    }

    /**
     * @brief Submits a task with default (Normal) priority
     */
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) {
        return submit(Priority::Normal, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /**
     * @brief Gets number of worker threads
     */
    [[nodiscard]] size_t num_threads() const noexcept {
        return workers_.size();
    }

    /**
     * @brief Gets approximate number of pending tasks
     */
    [[nodiscard]] size_t pending_tasks() const noexcept;

    /**
     * @brief Waits for all tasks to complete
     */
    void wait_all();

    /**
     * @brief Stops accepting new tasks and waits for current tasks
     */
    void shutdown();

    /**
     * @brief Sets CPU affinity for a worker thread
     */
    static bool set_thread_affinity(std::thread& thread, int cpu);

private:
    struct Worker {
        std::thread thread;
        WorkStealingDeque<Task> local_queue;
        std::atomic<bool> active{true};
        int cpu_id{-1};
    };

    void worker_loop(size_t worker_id);
    std::optional<Task> get_task(size_t worker_id);
    std::optional<Task> steal_task(size_t worker_id);

    std::vector<std::unique_ptr<Worker>> workers_;
    std::priority_queue<PrioritizedTask> global_queue_;
    std::mutex global_mutex_;
    std::condition_variable cv_;

    std::atomic<bool> running_{true};
    std::atomic<bool> shutdown_{false};
    std::atomic<uint64_t> task_sequence_{0};
    std::atomic<size_t> active_workers_{0};

    bool pin_to_cpu_;
    std::mt19937 rng_;
};

/**
 * @brief CPU affinity utilities
 */
namespace affinity {

/**
 * @brief Gets number of available CPU cores
 */
[[nodiscard]] size_t num_cpus() noexcept;

/**
 * @brief Sets CPU affinity for current thread
 */
bool set_affinity(int cpu);

/**
 * @brief Sets CPU affinity mask for current thread
 */
bool set_affinity_mask(const std::vector<int>& cpus);

/**
 * @brief Gets current thread's CPU affinity
 */
[[nodiscard]] std::vector<int> get_affinity();

/**
 * @brief Pins current thread to specific CPU
 */
bool pin_to_cpu(int cpu);

/**
 * @brief Isolates CPUs for HFT workload (Linux-specific)
 *
 * Attempts to move system processes off specified CPUs.
 */
bool isolate_cpus(const std::vector<int>& cpus);

}  // namespace affinity

/**
 * @brief RAII helper to run code on specific CPU
 */
class ScopedAffinity {
public:
    explicit ScopedAffinity(int cpu);
    ~ScopedAffinity();

    ScopedAffinity(const ScopedAffinity&) = delete;
    ScopedAffinity& operator=(const ScopedAffinity&) = delete;

private:
    std::vector<int> original_affinity_;
    bool valid_;
};

}  // namespace hft::core
