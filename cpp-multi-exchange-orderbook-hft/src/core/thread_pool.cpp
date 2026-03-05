/**
 * @file thread_pool.cpp
 * @brief Implementation of work-stealing thread pool
 */

#include "core/thread_pool.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace hft::core {

// ThreadPool implementation

ThreadPool::ThreadPool(size_t num_threads, bool pin_to_cpu)
    : pin_to_cpu_(pin_to_cpu)
    , rng_(std::random_device{}()) {

    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) {
            num_threads = 4;  // Fallback
        }
    }

    workers_.reserve(num_threads);

    for (size_t i = 0; i < num_threads; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->cpu_id = pin_to_cpu ? static_cast<int>(i) : -1;
        workers_.push_back(std::move(worker));
    }

    // Start worker threads
    for (size_t i = 0; i < num_threads; ++i) {
        workers_[i]->thread = std::thread(&ThreadPool::worker_loop, this, i);

        if (pin_to_cpu_) {
            set_thread_affinity(workers_[i]->thread, static_cast<int>(i));
        }
    }

    active_workers_.store(num_threads, std::memory_order_relaxed);
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    if (shutdown_.exchange(true)) {
        return;  // Already shutting down
    }

    running_.store(false, std::memory_order_release);

    // Wake up all workers
    cv_.notify_all();

    // Join all threads
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
}

void ThreadPool::wait_all() {
    while (pending_tasks() > 0 || active_workers_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

size_t ThreadPool::pending_tasks() const noexcept {
    size_t count = 0;

    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(global_mutex_));
        count = global_queue_.size();
    }

    for (const auto& worker : workers_) {
        count += worker->local_queue.size();
    }

    return count;
}

void ThreadPool::worker_loop(size_t worker_id) {
    Worker& worker = *workers_[worker_id];

    while (running_.load(std::memory_order_acquire)) {
        auto task = get_task(worker_id);

        if (task) {
            active_workers_.fetch_add(1, std::memory_order_relaxed);
            try {
                (*task)();
            } catch (...) {
                // Log error but don't crash
                LOG_ERROR("Task threw exception in worker %zu", worker_id);
            }
            active_workers_.fetch_sub(1, std::memory_order_relaxed);
        } else {
            // No work available, wait
            std::unique_lock<std::mutex> lock(global_mutex_);
            cv_.wait_for(lock, std::chrono::microseconds(100), [this] {
                return !running_.load(std::memory_order_relaxed) ||
                       !global_queue_.empty();
            });
        }
    }

    // Drain remaining tasks from local queue
    while (auto task = worker.local_queue.pop()) {
        try {
            (*task)();
        } catch (...) {
            LOG_ERROR("Task threw exception during shutdown in worker %zu", worker_id);
        }
    }
}

std::optional<Task> ThreadPool::get_task(size_t worker_id) {
    Worker& worker = *workers_[worker_id];

    // First try local queue
    if (auto task = worker.local_queue.pop()) {
        return task;
    }

    // Try global queue
    {
        std::lock_guard<std::mutex> lock(global_mutex_);
        if (!global_queue_.empty()) {
            PrioritizedTask ptask = std::move(const_cast<PrioritizedTask&>(global_queue_.top()));
            global_queue_.pop();
            return std::move(ptask.task);
        }
    }

    // Try stealing from other workers
    return steal_task(worker_id);
}

std::optional<Task> ThreadPool::steal_task(size_t worker_id) {
    size_t num_workers = workers_.size();
    if (num_workers <= 1) {
        return std::nullopt;
    }

    // Start from random worker to avoid contention patterns
    std::uniform_int_distribution<size_t> dist(0, num_workers - 1);
    size_t start = dist(rng_);

    for (size_t i = 0; i < num_workers; ++i) {
        size_t target = (start + i) % num_workers;
        if (target == worker_id) {
            continue;
        }

        if (auto task = workers_[target]->local_queue.steal()) {
            return task;
        }
    }

    return std::nullopt;
}

bool ThreadPool::set_thread_affinity(std::thread& thread, int cpu) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    int result = pthread_setaffinity_np(thread.native_handle(),
                                        sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        LOG_WARN("Failed to set thread affinity to CPU %d: %s", cpu, strerror(result));
        return false;
    }
    return true;
#elif defined(_WIN32)
    DWORD_PTR mask = 1ULL << cpu;
    DWORD_PTR result = SetThreadAffinityMask(
        reinterpret_cast<HANDLE>(thread.native_handle()), mask);
    if (result == 0) {
        LOG_WARN("Failed to set thread affinity to CPU %d", cpu);
        return false;
    }
    return true;
#else
    (void)thread;
    (void)cpu;
    LOG_WARN("Thread affinity not supported on this platform");
    return false;
#endif
}

// Affinity namespace implementation

namespace affinity {

size_t num_cpus() noexcept {
    size_t count = std::thread::hardware_concurrency();
    if (count == 0) {
#ifdef __linux__
        count = static_cast<size_t>(sysconf(_SC_NPROCESSORS_ONLN));
#endif
    }
    return count > 0 ? count : 1;
}

bool set_affinity(int cpu) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    int result = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    return result == 0;
#elif defined(_WIN32)
    DWORD_PTR mask = 1ULL << cpu;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
    (void)cpu;
    return false;
#endif
}

bool set_affinity_mask(const std::vector<int>& cpus) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int cpu : cpus) {
        CPU_SET(cpu, &cpuset);
    }

    int result = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    return result == 0;
#elif defined(_WIN32)
    DWORD_PTR mask = 0;
    for (int cpu : cpus) {
        mask |= (1ULL << cpu);
    }
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
    (void)cpus;
    return false;
#endif
}

std::vector<int> get_affinity() {
    std::vector<int> result;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    if (sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        for (int i = 0; i < CPU_SETSIZE; ++i) {
            if (CPU_ISSET(i, &cpuset)) {
                result.push_back(i);
            }
        }
    }
#elif defined(_WIN32)
    DWORD_PTR process_mask, system_mask;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        for (int i = 0; i < 64; ++i) {
            if (process_mask & (1ULL << i)) {
                result.push_back(i);
            }
        }
    }
#endif

    return result;
}

bool pin_to_cpu(int cpu) {
    return set_affinity(cpu);
}

bool isolate_cpus(const std::vector<int>& cpus) {
#ifdef __linux__
    // This requires root privileges and kernel configuration
    // (isolcpus boot parameter or cgroups)
    // Here we just try to set affinity, actual isolation needs system config

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int cpu : cpus) {
        CPU_SET(cpu, &cpuset);
    }

    // This won't truly isolate, just set affinity
    // True isolation requires: isolcpus=<cpus> boot parameter
    // or using cgroups cpuset controller
    LOG_WARN("CPU isolation requires kernel configuration (isolcpus or cgroups)");
    return sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)cpus;
    LOG_WARN("CPU isolation not supported on this platform");
    return false;
#endif
}

}  // namespace affinity

// ScopedAffinity implementation

ScopedAffinity::ScopedAffinity(int cpu)
    : original_affinity_(affinity::get_affinity())
    , valid_(false) {

    if (!original_affinity_.empty()) {
        valid_ = affinity::pin_to_cpu(cpu);
    }
}

ScopedAffinity::~ScopedAffinity() {
    if (valid_ && !original_affinity_.empty()) {
        affinity::set_affinity_mask(original_affinity_);
    }
}

}  // namespace hft::core
