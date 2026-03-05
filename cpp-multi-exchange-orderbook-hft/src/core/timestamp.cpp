/**
 * @file timestamp.cpp
 * @brief Implementation of timestamp utilities
 */

#include "core/timestamp.hpp"

#include <ctime>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>
#include <algorithm>

#ifdef __linux__
#include <time.h>
#endif

namespace hft::core {

// Static member definitions
std::atomic<uint64_t> RDTSCClock::frequency_{0};
std::atomic<double> RDTSCClock::nanos_per_cycle_{1.0};
std::atomic<bool> RDTSCClock::calibrated_{false};

uint64_t RDTSCClock::get_frequency() noexcept {
    return frequency_.load(std::memory_order_relaxed);
}

double RDTSCClock::get_nanos_per_cycle() noexcept {
    return nanos_per_cycle_.load(std::memory_order_relaxed);
}

uint64_t RDTSCClock::cycles_to_nanos(uint64_t cycles) noexcept {
    return static_cast<uint64_t>(
        static_cast<double>(cycles) * nanos_per_cycle_.load(std::memory_order_relaxed)
    );
}

uint64_t RDTSCClock::nanos_to_cycles(uint64_t nanos) noexcept {
    double npc = nanos_per_cycle_.load(std::memory_order_relaxed);
    if (npc <= 0.0) return nanos;
    return static_cast<uint64_t>(static_cast<double>(nanos) / npc);
}

bool RDTSCClock::is_calibrated() noexcept {
    return calibrated_.load(std::memory_order_relaxed);
}

void RDTSCClock::calibrate() {
    constexpr int NUM_SAMPLES = 10;
    constexpr int CALIBRATION_MS = 10;

    uint64_t total_cycles = 0;
    uint64_t total_nanos = 0;

    for (int i = 0; i < NUM_SAMPLES; ++i) {
        // Get start times
        cpuid_serialize();
        uint64_t start_tsc = rdtsc();
        auto start_time = std::chrono::steady_clock::now();

        // Sleep for calibration period
        std::this_thread::sleep_for(std::chrono::milliseconds(CALIBRATION_MS));

        // Get end times
        cpuid_serialize();
        uint64_t end_tsc = rdtsc();
        auto end_time = std::chrono::steady_clock::now();

        // Calculate deltas
        uint64_t delta_cycles = end_tsc - start_tsc;
        uint64_t delta_nanos = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end_time - start_time
            ).count()
        );

        total_cycles += delta_cycles;
        total_nanos += delta_nanos;
    }

    // Calculate frequency and nanos per cycle
    double nanos_per_cycle = static_cast<double>(total_nanos) / static_cast<double>(total_cycles);
    uint64_t frequency = static_cast<uint64_t>(
        static_cast<double>(total_cycles) * 1'000'000'000.0 / static_cast<double>(total_nanos)
    );

    nanos_per_cycle_.store(nanos_per_cycle, std::memory_order_relaxed);
    frequency_.store(frequency, std::memory_order_relaxed);
    calibrated_.store(true, std::memory_order_release);
}

// HighResTimestamp implementation

HighResTimestamp HighResTimestamp::now() noexcept {
    return now_monotonic();
}

HighResTimestamp HighResTimestamp::now_monotonic() noexcept {
#ifdef __linux__
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return HighResTimestamp{
        static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
        static_cast<uint64_t>(ts.tv_nsec)
    };
#else
    auto now = std::chrono::steady_clock::now();
    return HighResTimestamp{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()
            ).count()
        )
    };
#endif
}

HighResTimestamp HighResTimestamp::now_realtime() noexcept {
#ifdef __linux__
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return HighResTimestamp{
        static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
        static_cast<uint64_t>(ts.tv_nsec)
    };
#else
    auto now = std::chrono::system_clock::now();
    return HighResTimestamp{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()
            ).count()
        )
    };
#endif
}

HighResTimestamp HighResTimestamp::from_rdtsc() noexcept {
    uint64_t cycles = RDTSCClock::rdtsc();
    return HighResTimestamp{RDTSCClock::cycles_to_nanos(cycles)};
}

// LatencyTracker implementation

LatencyTracker::LatencyTracker() noexcept {
    reset();
}

void LatencyTracker::record(uint64_t nanos) noexcept {
    // Update min/max
    if (nanos < min_) min_ = nanos;
    if (nanos > max_) max_ = nanos;

    // Update sum and count
    sum_ += nanos;
    ++count_;

    // Update histogram
    size_t bucket = nanos / BUCKET_SIZE_NANOS;
    if (bucket < HISTOGRAM_BUCKETS) {
        ++histogram_[bucket];
    } else {
        ++overflow_count_;
    }
}

double LatencyTracker::mean() const noexcept {
    if (count_ == 0) return 0.0;
    return static_cast<double>(sum_) / static_cast<double>(count_);
}

uint64_t LatencyTracker::percentile(double p) const noexcept {
    if (count_ == 0) return 0;

    uint64_t target = static_cast<uint64_t>(static_cast<double>(count_) * p);
    uint64_t cumulative = 0;

    for (size_t i = 0; i < HISTOGRAM_BUCKETS; ++i) {
        cumulative += histogram_[i];
        if (cumulative >= target) {
            return (i + 1) * BUCKET_SIZE_NANOS;
        }
    }

    // In overflow bucket, return max
    return max_;
}

void LatencyTracker::reset() noexcept {
    min_ = std::numeric_limits<uint64_t>::max();
    max_ = 0;
    sum_ = 0;
    count_ = 0;
    histogram_.fill(0);
    overflow_count_ = 0;
}

// Utility functions

std::string format_timestamp_iso(const HighResTimestamp& ts) {
    uint64_t epoch_nanos = ts.nanos();
    time_t seconds = static_cast<time_t>(epoch_nanos / 1'000'000'000ULL);
    uint64_t nanos = epoch_nanos % 1'000'000'000ULL;

    struct tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &seconds);
#else
    gmtime_r(&seconds, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(9) << nanos << 'Z';
    return oss.str();
}

std::string format_timestamp_time(const HighResTimestamp& ts) {
    uint64_t epoch_nanos = ts.nanos();
    time_t seconds = static_cast<time_t>(epoch_nanos / 1'000'000'000ULL);
    uint64_t micros = (epoch_nanos % 1'000'000'000ULL) / 1000;

    struct tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &seconds);
#else
    gmtime_r(&seconds, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(6) << micros;
    return oss.str();
}

std::string format_duration(uint64_t nanos) {
    std::ostringstream oss;

    if (nanos < 1000) {
        oss << nanos << "ns";
    } else if (nanos < 1'000'000) {
        oss << std::fixed << std::setprecision(2)
            << (static_cast<double>(nanos) / 1000.0) << "us";
    } else if (nanos < 1'000'000'000) {
        oss << std::fixed << std::setprecision(2)
            << (static_cast<double>(nanos) / 1'000'000.0) << "ms";
    } else {
        oss << std::fixed << std::setprecision(2)
            << (static_cast<double>(nanos) / 1'000'000'000.0) << "s";
    }

    return oss.str();
}

HighResTimestamp parse_timestamp_iso(const std::string& str) {
    // Parse ISO 8601 format: YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ
    struct tm tm_buf = {};
    int nanos = 0;

    // Find the decimal point for fractional seconds
    size_t dot_pos = str.find('.');
    std::string datetime_part = str.substr(0, dot_pos);

    // Parse the datetime part
#ifdef _WIN32
    std::istringstream ss(datetime_part);
    ss >> std::get_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
#else
    strptime(datetime_part.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_buf);
#endif

    // Parse fractional seconds if present
    if (dot_pos != std::string::npos) {
        size_t end_pos = str.find('Z', dot_pos);
        if (end_pos == std::string::npos) {
            end_pos = str.length();
        }
        std::string frac = str.substr(dot_pos + 1, end_pos - dot_pos - 1);

        // Pad or truncate to 9 digits (nanoseconds)
        while (frac.length() < 9) frac += '0';
        frac = frac.substr(0, 9);

        nanos = std::stoi(frac);
    }

#ifdef _WIN32
    time_t seconds = _mkgmtime(&tm_buf);
#else
    time_t seconds = timegm(&tm_buf);
#endif

    return HighResTimestamp{
        static_cast<uint64_t>(seconds) * 1'000'000'000ULL +
        static_cast<uint64_t>(nanos)
    };
}

uint64_t epoch_nanos() noexcept {
    return HighResTimestamp::now_realtime().nanos();
}

uint64_t epoch_micros() noexcept {
    return HighResTimestamp::now_realtime().micros();
}

uint64_t epoch_millis() noexcept {
    return HighResTimestamp::now_realtime().millis();
}

void busy_sleep_nanos(uint64_t nanos) noexcept {
    if (!RDTSCClock::is_calibrated()) {
        // Fallback to chrono-based busy wait
        auto start = std::chrono::steady_clock::now();
        auto target = start + std::chrono::nanoseconds(nanos);
        while (std::chrono::steady_clock::now() < target) {
            // Busy wait with pause hint
#if defined(__x86_64__) || defined(_M_X64)
            __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
            __asm__ __volatile__("yield" ::: "memory");
#endif
        }
        return;
    }

    uint64_t start_cycles = RDTSCClock::rdtsc();
    uint64_t target_cycles = RDTSCClock::nanos_to_cycles(nanos);
    uint64_t end_cycles = start_cycles + target_cycles;

    while (RDTSCClock::rdtsc() < end_cycles) {
        // CPU pause hint to reduce power consumption
#if defined(__x86_64__) || defined(_M_X64)
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#endif
    }
}

void busy_sleep_micros(uint64_t micros) noexcept {
    busy_sleep_nanos(micros * 1000);
}

}  // namespace hft::core
