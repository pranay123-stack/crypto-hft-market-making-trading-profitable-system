/**
 * @file logger.cpp
 * @brief Implementation of the lock-free async logger
 */

#include "core/logger.hpp"
#include "core/timestamp.hpp"

#include <iostream>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>

#ifdef __linux__
#include <time.h>
#endif

namespace hft::core {

// ConsoleSink implementation

ConsoleSink::ConsoleSink(bool use_colors) noexcept : use_colors_(use_colors) {}

void ConsoleSink::write(const LogMessage& msg) {
    // Format timestamp
    uint64_t secs = msg.timestamp_nanos / 1'000'000'000ULL;
    uint64_t micros = (msg.timestamp_nanos % 1'000'000'000ULL) / 1000;

    time_t time_secs = static_cast<time_t>(secs);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time_secs);
#else
    localtime_r(&time_secs, &tm_buf);
#endif

    char time_str[32];
    std::strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);

    // Extract filename from path
    std::string_view file(msg.file.data(), msg.file_len);
    auto last_slash = file.find_last_of("/\\");
    if (last_slash != std::string_view::npos) {
        file = file.substr(last_slash + 1);
    }

    // Build output string
    std::ostringstream oss;

    if (use_colors_) {
        oss << log_level_color(msg.level);
    }

    oss << time_str << '.' << std::setfill('0') << std::setw(6) << micros
        << " [" << log_level_to_string(msg.level) << "] "
        << "[" << file << ":" << msg.line << "] "
        << std::string_view(msg.message.data(), msg.message_len);

    if (use_colors_) {
        oss << "\033[0m";  // Reset color
    }

    oss << '\n';

    // Write to stderr for errors, stdout for others
    if (msg.level >= LogLevel::Error) {
        std::cerr << oss.str();
    } else {
        std::cout << oss.str();
    }
}

void ConsoleSink::flush() {
    std::cout.flush();
    std::cerr.flush();
}

// FileSink implementation

FileSink::FileSink(const std::string& filename, bool append)
    : filename_(filename) {
    auto mode = std::ios::out;
    if (append) {
        mode |= std::ios::app;
    }
    file_.open(filename, mode);
    if (!file_.is_open()) {
        throw std::runtime_error("Failed to open log file: " + filename);
    }
}

FileSink::~FileSink() {
    if (file_.is_open()) {
        file_.close();
    }
}

void FileSink::write(const LogMessage& msg) {
    // Format timestamp
    uint64_t secs = msg.timestamp_nanos / 1'000'000'000ULL;
    uint64_t micros = (msg.timestamp_nanos % 1'000'000'000ULL) / 1000;

    time_t time_secs = static_cast<time_t>(secs);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time_secs);
#else
    localtime_r(&time_secs, &tm_buf);
#endif

    char time_str[32];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    // Extract filename from path
    std::string_view file(msg.file.data(), msg.file_len);
    auto last_slash = file.find_last_of("/\\");
    if (last_slash != std::string_view::npos) {
        file = file.substr(last_slash + 1);
    }

    file_ << time_str << '.' << std::setfill('0') << std::setw(6) << micros
          << " [" << log_level_to_string(msg.level) << "] "
          << "[" << file << ":" << msg.line << "] "
          << std::string_view(msg.message.data(), msg.message_len)
          << '\n';
}

void FileSink::flush() {
    file_.flush();
}

// RotatingFileSink implementation

RotatingFileSink::RotatingFileSink(const std::string& base_filename,
                                   size_t max_size_bytes,
                                   size_t max_files)
    : base_filename_(base_filename)
    , max_size_(max_size_bytes)
    , max_files_(max_files)
    , current_size_(0) {

    // Open the first file
    file_.open(get_filename(0), std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        throw std::runtime_error("Failed to open log file: " + get_filename(0));
    }

    // Get current file size
    file_.seekp(0, std::ios::end);
    current_size_ = static_cast<size_t>(file_.tellp());
}

RotatingFileSink::~RotatingFileSink() {
    if (file_.is_open()) {
        file_.close();
    }
}

void RotatingFileSink::write(const LogMessage& msg) {
    // Check if rotation is needed
    if (current_size_ >= max_size_) {
        rotate();
    }

    // Format timestamp
    uint64_t secs = msg.timestamp_nanos / 1'000'000'000ULL;
    uint64_t micros = (msg.timestamp_nanos % 1'000'000'000ULL) / 1000;

    time_t time_secs = static_cast<time_t>(secs);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time_secs);
#else
    localtime_r(&time_secs, &tm_buf);
#endif

    char time_str[32];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    // Extract filename from path
    std::string_view file(msg.file.data(), msg.file_len);
    auto last_slash = file.find_last_of("/\\");
    if (last_slash != std::string_view::npos) {
        file = file.substr(last_slash + 1);
    }

    std::ostringstream oss;
    oss << time_str << '.' << std::setfill('0') << std::setw(6) << micros
        << " [" << log_level_to_string(msg.level) << "] "
        << "[" << file << ":" << msg.line << "] "
        << std::string_view(msg.message.data(), msg.message_len)
        << '\n';

    std::string line = oss.str();
    file_ << line;
    current_size_ += line.size();
}

void RotatingFileSink::flush() {
    file_.flush();
}

void RotatingFileSink::rotate() {
    file_.close();

    // Delete oldest file if at max
    std::string oldest = get_filename(max_files_ - 1);
    if (std::filesystem::exists(oldest)) {
        std::filesystem::remove(oldest);
    }

    // Rename existing files
    for (size_t i = max_files_ - 2; i > 0; --i) {
        std::string old_name = get_filename(i);
        std::string new_name = get_filename(i + 1);
        if (std::filesystem::exists(old_name)) {
            std::filesystem::rename(old_name, new_name);
        }
    }

    // Rename current file
    std::string current = get_filename(0);
    if (std::filesystem::exists(current)) {
        std::filesystem::rename(current, get_filename(1));
    }

    // Open new file
    file_.open(current, std::ios::out);
    current_size_ = 0;
}

std::string RotatingFileSink::get_filename(size_t index) const {
    if (index == 0) {
        return base_filename_;
    }
    return base_filename_ + "." + std::to_string(index);
}

// Logger implementation

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger()
    : min_level_(LogLevel::Info)
    , running_(false)
    , initialized_(false)
    , dropped_count_(0) {
}

Logger::~Logger() {
    shutdown();
}

void Logger::init(LogLevel min_level, bool console_output, const std::string& file_path) {
    if (initialized_.exchange(true)) {
        return;  // Already initialized
    }

    min_level_.store(min_level, std::memory_order_relaxed);

    // Add default sinks
    if (console_output) {
        add_sink(std::make_unique<ConsoleSink>(true));
    }

    if (!file_path.empty()) {
        add_sink(std::make_unique<FileSink>(file_path));
    }

    // Start background thread
    running_.store(true, std::memory_order_release);
    background_thread_ = std::thread(&Logger::background_thread_func, this);
}

void Logger::add_sink(std::unique_ptr<LogSink> sink) {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::set_level(LogLevel level) noexcept {
    min_level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::get_level() const noexcept {
    return min_level_.load(std::memory_order_relaxed);
}

bool Logger::is_enabled(LogLevel level) const noexcept {
    return level >= min_level_.load(std::memory_order_relaxed);
}

void Logger::log(LogLevel level,
                 const char* file,
                 const char* function,
                 uint32_t line,
                 const char* format,
                 ...) noexcept {
    if (!is_enabled(level)) {
        return;
    }

    LogMessage msg;
    msg.timestamp_nanos = epoch_nanos();
    msg.level = level;
    msg.line = line;
    msg.thread_id = std::this_thread::get_id();

    // Copy file name
    size_t file_len = std::strlen(file);
    msg.file_len = static_cast<uint16_t>(
        std::min(file_len, LogMessage::MAX_FILE_SIZE - 1));
    std::memcpy(msg.file.data(), file, msg.file_len);
    msg.file[msg.file_len] = '\0';

    // Copy function name
    size_t func_len = std::strlen(function);
    msg.func_len = static_cast<uint16_t>(
        std::min(func_len, LogMessage::MAX_FUNC_SIZE - 1));
    std::memcpy(msg.function.data(), function, msg.func_len);
    msg.function[msg.func_len] = '\0';

    // Format message
    va_list args;
    va_start(args, format);
    int len = std::vsnprintf(msg.message.data(), LogMessage::MAX_MESSAGE_SIZE,
                             format, args);
    va_end(args);

    if (len < 0) {
        msg.message_len = 0;
    } else {
        msg.message_len = static_cast<uint16_t>(
            std::min(static_cast<size_t>(len), LogMessage::MAX_MESSAGE_SIZE - 1));
    }

    // Try to enqueue
    if (!queue_.try_push(std::move(msg))) {
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::log_raw(LogLevel level,
                     const char* file,
                     const char* function,
                     uint32_t line,
                     std::string_view message) noexcept {
    if (!is_enabled(level)) {
        return;
    }

    LogMessage msg;
    msg.timestamp_nanos = epoch_nanos();
    msg.level = level;
    msg.line = line;
    msg.thread_id = std::this_thread::get_id();

    // Copy file name
    size_t file_len = std::strlen(file);
    msg.file_len = static_cast<uint16_t>(
        std::min(file_len, LogMessage::MAX_FILE_SIZE - 1));
    std::memcpy(msg.file.data(), file, msg.file_len);
    msg.file[msg.file_len] = '\0';

    // Copy function name
    size_t func_len = std::strlen(function);
    msg.func_len = static_cast<uint16_t>(
        std::min(func_len, LogMessage::MAX_FUNC_SIZE - 1));
    std::memcpy(msg.function.data(), function, msg.func_len);
    msg.function[msg.func_len] = '\0';

    // Copy message
    msg.message_len = static_cast<uint16_t>(
        std::min(message.size(), LogMessage::MAX_MESSAGE_SIZE - 1));
    std::memcpy(msg.message.data(), message.data(), msg.message_len);
    msg.message[msg.message_len] = '\0';

    // Try to enqueue
    if (!queue_.try_push(std::move(msg))) {
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::flush() {
    // Process all remaining messages
    LogMessage msg;
    while (queue_.try_pop(msg)) {
        process_message(msg);
    }

    // Flush all sinks
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    for (auto& sink : sinks_) {
        sink->flush();
    }
}

void Logger::shutdown() {
    if (!running_.exchange(false)) {
        return;  // Not running
    }

    // Wait for background thread
    if (background_thread_.joinable()) {
        background_thread_.join();
    }

    // Flush remaining messages
    flush();
}

size_t Logger::queue_size() const noexcept {
    return queue_.size_approx();
}

size_t Logger::dropped_count() const noexcept {
    return dropped_count_.load(std::memory_order_relaxed);
}

void Logger::background_thread_func() {
    LogMessage msg;

    while (running_.load(std::memory_order_acquire)) {
        // Try to pop and process messages
        while (queue_.try_pop(msg)) {
            process_message(msg);
        }

        // No messages, yield briefly
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Drain remaining messages
    while (queue_.try_pop(msg)) {
        process_message(msg);
    }
}

void Logger::process_message(const LogMessage& msg) {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    for (auto& sink : sinks_) {
        sink->write(msg);
    }
}

}  // namespace hft::core
