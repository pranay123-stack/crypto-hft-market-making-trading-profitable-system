#pragma once

#include "core/types.hpp"
#include <string>
#include <string_view>
#include <fstream>
#include <mutex>
#include <memory>
#include <atomic>
#include <array>
#include <cstdio>

namespace hft {

// ============================================================================
// Log Levels
// ============================================================================

enum class LogLevel : uint8_t {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    FATAL = 5,
    OFF = 6
};

inline const char* log_level_str(LogLevel level) {
    static const char* names[] = {
        "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"
    };
    return names[static_cast<int>(level)];
}

// ============================================================================
// Lock-Free Log Entry
// ============================================================================

struct alignas(128) LogEntry {
    Timestamp timestamp;
    LogLevel level;
    char message[119];  // Pad to 128 bytes

    LogEntry() : timestamp(0), level(LogLevel::INFO), message{0} {}
};

// ============================================================================
// High-Performance Logger
// ============================================================================

class Logger {
public:
    static constexpr size_t BUFFER_SIZE = 8192;

    explicit Logger(const std::string& name, LogLevel min_level = LogLevel::INFO);
    ~Logger();

    // Non-copyable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Logging methods
    template<typename... Args>
    void trace(const char* fmt, Args&&... args) {
        log(LogLevel::TRACE, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void debug(const char* fmt, Args&&... args) {
        log(LogLevel::DEBUG, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(const char* fmt, Args&&... args) {
        log(LogLevel::INFO, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(const char* fmt, Args&&... args) {
        log(LogLevel::WARN, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(const char* fmt, Args&&... args) {
        log(LogLevel::ERROR, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void fatal(const char* fmt, Args&&... args) {
        log(LogLevel::FATAL, fmt, std::forward<Args>(args)...);
    }

    // Configuration
    void set_level(LogLevel level) { min_level_ = level; }
    void set_file(const std::string& path);
    void set_console(bool enabled) { console_enabled_ = enabled; }
    void flush();

    // Statistics
    [[nodiscard]] uint64_t messages_logged() const { return messages_logged_; }
    [[nodiscard]] uint64_t messages_dropped() const { return messages_dropped_; }

private:
    template<typename... Args>
    void log(LogLevel level, const char* fmt, Args&&... args) {
        if (level < min_level_) return;

        char buffer[256];
        int len = snprintf(buffer, sizeof(buffer), fmt, std::forward<Args>(args)...);
        if (len > 0) {
            write_log(level, std::string_view(buffer, std::min<size_t>(len, sizeof(buffer) - 1)));
        }
    }

    void log(LogLevel level, const char* msg) {
        if (level < min_level_) return;
        write_log(level, msg);
    }

    void write_log(LogLevel level, std::string_view message);
    void flush_buffer();

    std::string name_;
    LogLevel min_level_;
    bool console_enabled_ = true;

    std::ofstream file_;
    std::mutex file_mutex_;

    std::array<LogEntry, BUFFER_SIZE> buffer_;
    std::atomic<size_t> write_pos_{0};
    std::atomic<size_t> read_pos_{0};

    std::atomic<uint64_t> messages_logged_{0};
    std::atomic<uint64_t> messages_dropped_{0};
};

// ============================================================================
// Global Logger Access
// ============================================================================

class LogManager {
public:
    static LogManager& instance();

    Logger& get(const std::string& name);
    Logger& default_logger();

    void set_global_level(LogLevel level);
    void set_log_directory(const std::string& dir);

private:
    LogManager() = default;
    std::unordered_map<std::string, std::unique_ptr<Logger>> loggers_;
    std::mutex mutex_;
    std::string log_dir_ = "./logs";
    LogLevel global_level_ = LogLevel::INFO;
};

// ============================================================================
// Convenience Macros
// ============================================================================

#define LOG_TRACE(...) hft::LogManager::instance().default_logger().trace(__VA_ARGS__)
#define LOG_DEBUG(...) hft::LogManager::instance().default_logger().debug(__VA_ARGS__)
#define LOG_INFO(...)  hft::LogManager::instance().default_logger().info(__VA_ARGS__)
#define LOG_WARN(...)  hft::LogManager::instance().default_logger().warn(__VA_ARGS__)
#define LOG_ERROR(...) hft::LogManager::instance().default_logger().error(__VA_ARGS__)
#define LOG_FATAL(...) hft::LogManager::instance().default_logger().fatal(__VA_ARGS__)

} // namespace hft
