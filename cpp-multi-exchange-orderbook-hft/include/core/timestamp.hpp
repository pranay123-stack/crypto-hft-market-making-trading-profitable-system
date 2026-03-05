#pragma once

/**
 * @file timestamp.hpp
 * @brief Ultra-low latency timestamp utilities for HFT systems
 *
 * Provides nanosecond-precision timing using multiple clock sources:
 * - RDTSC (Read Time-Stamp Counter) for ultra-low latency relative timing
 * - clock_gettime(CLOCK_MONOTONIC) for stable monotonic timing
 * - clock_gettime(CLOCK_REALTIME) for wall-clock time
 *
 * Key features:
 * - RDTSC calibration for cycle-to-nanosecond conversion
 * - Sub-nanosecond overhead timing calls
 * - Timestamp arithmetic and comparison
 * - Time formatting utilities
 */

#include <cstdint>
#include <chrono>
#include <string>
#include <atomic>

#include "types.hpp"

namespace hft::core {

// Forward declarations
class TimestampClock;
class RDTSCClock;

/**
 * @brief RDTSC (Read Time-Stamp Counter) clock
 *
 * Provides ultra-low latency timing using CPU cycle counter.
 * Must be calibrated against a reference clock for accurate conversion.
 */
class RDTSCClock {
public:
    /**
     * @brief Reads the CPU timestamp counter
     *
     * Uses RDTSC instruction for minimal overhead (~10-20 cycles).
     */
    [[nodiscard]] static inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        uint32_t lo, hi;
        __asm__ __volatile__(
            "rdtsc"
            : "=a"(lo), "=d"(hi)
        );
        return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
        // ARM64: use CNTVCT_EL0 (virtual count register)
        uint64_t val;
        __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
        return val;
#else
        // Fallback to chrono
        return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
    }

    /**
     * @brief Reads RDTSC with serializing instruction
     *
     * Uses RDTSCP for ordered reads (prevents instruction reordering).
     * Slightly higher latency than rdtsc() but more accurate for measurements.
     */
    [[nodiscard]] static inline uint64_t rdtscp() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        uint32_t lo, hi, aux;
        __asm__ __volatile__(
            "rdtscp"
            : "=a"(lo), "=d"(hi), "=c"(aux)
        );
        return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
        // ARM64: ISB ensures completion of previous instructions
        __asm__ __volatile__("isb");
        uint64_t val;
        __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
        return val;
#else
        return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
    }

    /**
     * @brief Serializing fence before measurement
     *
     * Use before rdtsc() to prevent preceding instructions from
     * being reordered past the timestamp read.
     */
    static inline void fence() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        __asm__ __volatile__("mfence" ::: "memory");
#elif defined(__aarch64__)
        __asm__ __volatile__("dmb sy" ::: "memory");
#else
        std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
    }

    /**
     * @brief CPU ID instruction to serialize
     *
     * More heavyweight serialization than fence().
     */
    static inline void cpuid_serialize() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        uint32_t eax, ebx, ecx, edx;
        __asm__ __volatile__(
            "cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(0)
        );
#elif defined(__aarch64__)
        __asm__ __volatile__("isb" ::: "memory");
#endif
    }

    /**
     * @brief Gets CPU frequency in Hz (cycles per second)
     *
     * This is calibrated once at initialization.
     */
    [[nodiscard]] static uint64_t get_frequency() noexcept;

    /**
     * @brief Gets nanoseconds per cycle (for conversion)
     */
    [[nodiscard]] static double get_nanos_per_cycle() noexcept;

    /**
     * @brief Converts cycles to nanoseconds
     */
    [[nodiscard]] static uint64_t cycles_to_nanos(uint64_t cycles) noexcept;

    /**
     * @brief Converts nanoseconds to cycles
     */
    [[nodiscard]] static uint64_t nanos_to_cycles(uint64_t nanos) noexcept;

    /**
     * @brief Calibrates RDTSC against reference clock
     *
     * Should be called once at startup. Takes ~100ms.
     */
    static void calibrate();

    /**
     * @brief Checks if RDTSC is calibrated
     */
    [[nodiscard]] static bool is_calibrated() noexcept;

private:
    static std::atomic<uint64_t> frequency_;
    static std::atomic<double> nanos_per_cycle_;
    static std::atomic<bool> calibrated_;
};

/**
 * @brief High-precision timestamp class
 *
 * Wraps nanosecond timestamps with convenient operations.
 */
class HighResTimestamp {
public:
    constexpr HighResTimestamp() noexcept : nanos_(0) {}
    constexpr explicit HighResTimestamp(uint64_t nanos) noexcept : nanos_(nanos) {}

    // Factory methods
    [[nodiscard]] static HighResTimestamp now() noexcept;
    [[nodiscard]] static HighResTimestamp now_monotonic() noexcept;
    [[nodiscard]] static HighResTimestamp now_realtime() noexcept;
    [[nodiscard]] static HighResTimestamp from_rdtsc() noexcept;

    // Accessors
    [[nodiscard]] constexpr uint64_t nanos() const noexcept { return nanos_; }
    [[nodiscard]] constexpr uint64_t micros() const noexcept { return nanos_ / 1000; }
    [[nodiscard]] constexpr uint64_t millis() const noexcept { return nanos_ / 1'000'000; }
    [[nodiscard]] constexpr uint64_t seconds() const noexcept { return nanos_ / 1'000'000'000; }

    // Conversion to Timestamp type
    [[nodiscard]] constexpr Timestamp to_timestamp() const noexcept {
        return Timestamp{nanos_};
    }

    // Comparison
    [[nodiscard]] constexpr auto operator<=>(const HighResTimestamp&) const noexcept = default;

    // Arithmetic
    [[nodiscard]] constexpr HighResTimestamp operator+(const HighResTimestamp& other) const noexcept {
        return HighResTimestamp{nanos_ + other.nanos_};
    }

    [[nodiscard]] constexpr HighResTimestamp operator-(const HighResTimestamp& other) const noexcept {
        return HighResTimestamp{nanos_ - other.nanos_};
    }

    HighResTimestamp& operator+=(const HighResTimestamp& other) noexcept {
        nanos_ += other.nanos_;
        return *this;
    }

    HighResTimestamp& operator-=(const HighResTimestamp& other) noexcept {
        nanos_ -= other.nanos_;
        return *this;
    }

    // Duration helpers
    [[nodiscard]] static constexpr HighResTimestamp from_nanos(uint64_t n) noexcept {
        return HighResTimestamp{n};
    }

    [[nodiscard]] static constexpr HighResTimestamp from_micros(uint64_t us) noexcept {
        return HighResTimestamp{us * 1000};
    }

    [[nodiscard]] static constexpr HighResTimestamp from_millis(uint64_t ms) noexcept {
        return HighResTimestamp{ms * 1'000'000};
    }

    [[nodiscard]] static constexpr HighResTimestamp from_seconds(uint64_t s) noexcept {
        return HighResTimestamp{s * 1'000'000'000};
    }

    // Check validity
    [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return nanos_ != 0; }

private:
    uint64_t nanos_;
};

/**
 * @brief Scoped timer for measuring elapsed time
 *
 * RAII helper that measures time between construction and query.
 */
class ScopedTimer {
public:
    ScopedTimer() noexcept : start_(RDTSCClock::rdtsc()) {}

    /**
     * @brief Returns elapsed nanoseconds since construction
     */
    [[nodiscard]] uint64_t elapsed_nanos() const noexcept {
        uint64_t end = RDTSCClock::rdtsc();
        return RDTSCClock::cycles_to_nanos(end - start_);
    }

    /**
     * @brief Returns elapsed microseconds
     */
    [[nodiscard]] uint64_t elapsed_micros() const noexcept {
        return elapsed_nanos() / 1000;
    }

    /**
     * @brief Returns elapsed milliseconds
     */
    [[nodiscard]] uint64_t elapsed_millis() const noexcept {
        return elapsed_nanos() / 1'000'000;
    }

    /**
     * @brief Resets the timer
     */
    void reset() noexcept {
        start_ = RDTSCClock::rdtsc();
    }

    /**
     * @brief Returns elapsed time and resets
     */
    [[nodiscard]] uint64_t lap_nanos() noexcept {
        uint64_t end = RDTSCClock::rdtsc();
        uint64_t elapsed = RDTSCClock::cycles_to_nanos(end - start_);
        start_ = end;
        return elapsed;
    }

private:
    uint64_t start_;
};

/**
 * @brief Latency statistics tracker
 *
 * Tracks min, max, average, and percentile latencies.
 */
class LatencyTracker {
public:
    static constexpr size_t HISTOGRAM_BUCKETS = 1024;
    static constexpr uint64_t BUCKET_SIZE_NANOS = 100;  // 100ns per bucket

    LatencyTracker() noexcept;

    /**
     * @brief Records a latency sample
     */
    void record(uint64_t nanos) noexcept;

    /**
     * @brief Records elapsed time from a ScopedTimer
     */
    void record(const ScopedTimer& timer) noexcept {
        record(timer.elapsed_nanos());
    }

    // Statistics
    [[nodiscard]] uint64_t min() const noexcept { return min_; }
    [[nodiscard]] uint64_t max() const noexcept { return max_; }
    [[nodiscard]] uint64_t count() const noexcept { return count_; }
    [[nodiscard]] double mean() const noexcept;
    [[nodiscard]] uint64_t percentile(double p) const noexcept;

    // Common percentiles
    [[nodiscard]] uint64_t p50() const noexcept { return percentile(0.50); }
    [[nodiscard]] uint64_t p90() const noexcept { return percentile(0.90); }
    [[nodiscard]] uint64_t p95() const noexcept { return percentile(0.95); }
    [[nodiscard]] uint64_t p99() const noexcept { return percentile(0.99); }
    [[nodiscard]] uint64_t p999() const noexcept { return percentile(0.999); }

    /**
     * @brief Resets all statistics
     */
    void reset() noexcept;

private:
    uint64_t min_;
    uint64_t max_;
    uint64_t sum_;
    uint64_t count_;
    std::array<uint64_t, HISTOGRAM_BUCKETS> histogram_;
    uint64_t overflow_count_;
};

// Utility functions

/**
 * @brief Formats timestamp as ISO 8601 string
 */
[[nodiscard]] std::string format_timestamp_iso(const HighResTimestamp& ts);

/**
 * @brief Formats timestamp as HH:MM:SS.microseconds
 */
[[nodiscard]] std::string format_timestamp_time(const HighResTimestamp& ts);

/**
 * @brief Formats duration in human-readable form
 */
[[nodiscard]] std::string format_duration(uint64_t nanos);

/**
 * @brief Parses ISO 8601 timestamp string
 */
[[nodiscard]] HighResTimestamp parse_timestamp_iso(const std::string& str);

/**
 * @brief Gets current time as epoch nanoseconds
 */
[[nodiscard]] uint64_t epoch_nanos() noexcept;

/**
 * @brief Gets current time as epoch microseconds
 */
[[nodiscard]] uint64_t epoch_micros() noexcept;

/**
 * @brief Gets current time as epoch milliseconds
 */
[[nodiscard]] uint64_t epoch_millis() noexcept;

/**
 * @brief Sleeps for specified nanoseconds (busy-wait for precision)
 */
void busy_sleep_nanos(uint64_t nanos) noexcept;

/**
 * @brief Sleeps for specified microseconds
 */
void busy_sleep_micros(uint64_t micros) noexcept;

}  // namespace hft::core
