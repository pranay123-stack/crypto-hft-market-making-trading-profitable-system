#pragma once

/**
 * @file logger.hpp
 * @brief High-performance lock-free async logger for HFT systems
 *
 * This logger is designed to minimize latency impact on the hot path:
 * - Lock-free SPSC queue for log messages
 * - Background thread handles actual I/O
 * - Microsecond-precision timestamps
 * - Multiple output sinks (file, console)
 * - Configurable log levels
 *
 * Usage:
 *   LOG_INFO("Order {} filled at price {}", order_id, price);
 *   LOG_ERROR("Connection failed: {}", error_msg);
 */

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <string_view>
#include <memory>
#include <thread>
#include <fstream>
#include <functional>
#include <source_location>

#include "types.hpp"
#include "lock_free_queue.hpp"

namespace hft::core {

/**
 * @brief Log severity levels
 */
enum class LogLevel : uint8_t {
    Trace = 0,   // Extremely detailed, typically disabled in production
    Debug = 1,   // Debugging information
    Info = 2,    // Normal operational messages
    Warn = 3,    // Warning conditions
    Error = 4,   // Error conditions
    Fatal = 5,   // Fatal errors, system should terminate
    Off = 6      // Logging disabled
};

[[nodiscard]] constexpr std::string_view log_level_to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        case LogLevel::Off:   return "OFF  ";
    }
    return "?????";
}

[[nodiscard]] constexpr std::string_view log_level_color(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "\033[90m";  // Dark gray
        case LogLevel::Debug: return "\033[36m";  // Cyan
        case LogLevel::Info:  return "\033[32m";  // Green
        case LogLevel::Warn:  return "\033[33m";  // Yellow
        case LogLevel::Error: return "\033[31m";  // Red
        case LogLevel::Fatal: return "\033[35m";  // Magenta
        default:              return "\033[0m";   // Reset
    }
}

/**
 * @brief Log message structure for the async queue
 */
struct LogMessage {
    static constexpr size_t MAX_MESSAGE_SIZE = 512;
    static constexpr size_t MAX_FILE_SIZE = 64;
    static constexpr size_t MAX_FUNC_SIZE = 64;

    uint64_t timestamp_nanos;
    LogLevel level;
    uint32_t line;
    std::array<char, MAX_MESSAGE_SIZE> message;
    std::array<char, MAX_FILE_SIZE> file;
    std::array<char, MAX_FUNC_SIZE> function;
    uint16_t message_len;
    uint16_t file_len;
    uint16_t func_len;
    std::thread::id thread_id;

    LogMessage() noexcept = default;
};

static_assert(sizeof(LogMessage) <= 768, "LogMessage too large");

/**
 * @brief Log sink interface
 */
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogMessage& msg) = 0;
    virtual void flush() = 0;
};

/**
 * @brief Console log sink with colors
 */
class ConsoleSink : public LogSink {
public:
    explicit ConsoleSink(bool use_colors = true) noexcept;
    void write(const LogMessage& msg) override;
    void flush() override;

private:
    bool use_colors_;
};

/**
 * @brief File log sink
 */
class FileSink : public LogSink {
public:
    explicit FileSink(const std::string& filename, bool append = true);
    ~FileSink() override;
    void write(const LogMessage& msg) override;
    void flush() override;

private:
    std::ofstream file_;
    std::string filename_;
};

/**
 * @brief Rotating file log sink
 *
 * Rotates files when they exceed max_size or at midnight.
 */
class RotatingFileSink : public LogSink {
public:
    RotatingFileSink(const std::string& base_filename,
                     size_t max_size_bytes = 100 * 1024 * 1024,  // 100MB
                     size_t max_files = 10);
    ~RotatingFileSink() override;
    void write(const LogMessage& msg) override;
    void flush() override;

private:
    void rotate();
    std::string get_filename(size_t index) const;

    std::string base_filename_;
    size_t max_size_;
    size_t max_files_;
    size_t current_size_;
    std::ofstream file_;
};

/**
 * @brief Lock-free async logger
 *
 * Singleton logger that processes log messages in a background thread.
 */
class Logger {
public:
    static constexpr size_t QUEUE_SIZE = 65536;  // Power of 2
    using Queue = SPSCQueue<LogMessage, QUEUE_SIZE>;

    /**
     * @brief Gets the singleton logger instance
     */
    static Logger& instance();

    /**
     * @brief Initializes the logger
     *
     * @param min_level Minimum log level to process
     * @param console_output Enable console output
     * @param file_path Optional log file path
     */
    void init(LogLevel min_level = LogLevel::Info,
              bool console_output = true,
              const std::string& file_path = "");

    /**
     * @brief Adds a custom log sink
     */
    void add_sink(std::unique_ptr<LogSink> sink);

    /**
     * @brief Sets the minimum log level
     */
    void set_level(LogLevel level) noexcept;

    /**
     * @brief Gets the current minimum log level
     */
    [[nodiscard]] LogLevel get_level() const noexcept;

    /**
     * @brief Checks if a log level is enabled
     */
    [[nodiscard]] bool is_enabled(LogLevel level) const noexcept;

    /**
     * @brief Logs a message (called by macros)
     */
    void log(LogLevel level,
             const char* file,
             const char* function,
             uint32_t line,
             const char* format,
             ...) noexcept;

    /**
     * @brief Logs a pre-formatted message
     */
    void log_raw(LogLevel level,
                 const char* file,
                 const char* function,
                 uint32_t line,
                 std::string_view message) noexcept;

    /**
     * @brief Flushes all pending messages
     */
    void flush();

    /**
     * @brief Shuts down the logger
     */
    void shutdown();

    /**
     * @brief Gets queue statistics
     */
    [[nodiscard]] size_t queue_size() const noexcept;
    [[nodiscard]] size_t dropped_count() const noexcept;

    // Deleted copy/move
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

private:
    Logger();
    ~Logger();

    void background_thread_func();
    void process_message(const LogMessage& msg);

    std::atomic<LogLevel> min_level_;
    std::atomic<bool> running_;
    std::atomic<bool> initialized_;
    std::atomic<uint64_t> dropped_count_;

    Queue queue_;
    std::thread background_thread_;

    std::vector<std::unique_ptr<LogSink>> sinks_;
    std::mutex sinks_mutex_;  // Only used for adding sinks, not writing
};

// ============================================================================
// Logging Macros
// ============================================================================

#define HFT_LOG(level, ...) \
    do { \
        if (::hft::core::Logger::instance().is_enabled(level)) { \
            ::hft::core::Logger::instance().log( \
                level, __FILE__, __func__, __LINE__, __VA_ARGS__); \
        } \
    } while (0)

#define LOG_TRACE(...) HFT_LOG(::hft::core::LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) HFT_LOG(::hft::core::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  HFT_LOG(::hft::core::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...)  HFT_LOG(::hft::core::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) HFT_LOG(::hft::core::LogLevel::Error, __VA_ARGS__)
#define LOG_FATAL(...) HFT_LOG(::hft::core::LogLevel::Fatal, __VA_ARGS__)

// Conditional logging (only evaluates arguments if enabled)
#define LOG_IF(level, condition, ...) \
    do { \
        if ((condition) && ::hft::core::Logger::instance().is_enabled(level)) { \
            ::hft::core::Logger::instance().log( \
                level, __FILE__, __func__, __LINE__, __VA_ARGS__); \
        } \
    } while (0)

// Rate-limited logging (logs at most once per interval)
#define LOG_EVERY_N(level, n, ...) \
    do { \
        static std::atomic<uint64_t> _log_counter{0}; \
        if ((_log_counter.fetch_add(1, std::memory_order_relaxed) % (n)) == 0 && \
            ::hft::core::Logger::instance().is_enabled(level)) { \
            ::hft::core::Logger::instance().log( \
                level, __FILE__, __func__, __LINE__, __VA_ARGS__); \
        } \
    } while (0)

// First N occurrences only
#define LOG_FIRST_N(level, n, ...) \
    do { \
        static std::atomic<uint64_t> _log_counter{0}; \
        uint64_t _count = _log_counter.fetch_add(1, std::memory_order_relaxed); \
        if (_count < (n) && ::hft::core::Logger::instance().is_enabled(level)) { \
            ::hft::core::Logger::instance().log( \
                level, __FILE__, __func__, __LINE__, __VA_ARGS__); \
        } \
    } while (0)

}  // namespace hft::core
