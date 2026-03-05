#pragma once

#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <atomic>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <thread>
#include <optional>

namespace hft {
namespace network {

// Connection state
enum class ConnectionState {
    Idle,
    InUse,
    Invalid,
    Connecting
};

// Health check result
struct HealthCheckResult {
    bool healthy{false};
    std::chrono::microseconds latency{0};
    std::string error;
    std::chrono::steady_clock::time_point timestamp;
};

// Connection wrapper base class
template <typename T>
class PooledConnection {
public:
    using Ptr = std::shared_ptr<PooledConnection<T>>;
    using RawPtr = T*;

    PooledConnection(std::unique_ptr<T> connection, size_t id)
        : connection_(std::move(connection)),
          id_(id),
          created_at_(std::chrono::steady_clock::now()),
          last_used_(created_at_),
          state_(ConnectionState::Idle) {}

    virtual ~PooledConnection() = default;

    // Get raw connection
    T* get() { return connection_.get(); }
    const T* get() const { return connection_.get(); }

    T* operator->() { return connection_.get(); }
    const T* operator->() const { return connection_.get(); }

    T& operator*() { return *connection_; }
    const T& operator*() const { return *connection_; }

    // State management
    ConnectionState state() const { return state_.load(); }
    void setState(ConnectionState state) { state_ = state; }

    // Timestamps
    std::chrono::steady_clock::time_point createdAt() const { return created_at_; }
    std::chrono::steady_clock::time_point lastUsed() const { return last_used_; }
    void touch() { last_used_ = std::chrono::steady_clock::now(); }

    // Statistics
    size_t id() const { return id_; }
    uint64_t useCount() const { return use_count_.load(); }
    void incrementUseCount() { use_count_++; }

    // Health tracking
    void setHealthy(bool healthy) { healthy_ = healthy; }
    bool isHealthy() const { return healthy_.load(); }

    // Age in milliseconds
    int64_t ageMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - created_at_
        ).count();
    }

    // Idle time in milliseconds
    int64_t idleMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_used_
        ).count();
    }

protected:
    std::unique_ptr<T> connection_;
    size_t id_;
    std::chrono::steady_clock::time_point created_at_;
    std::chrono::steady_clock::time_point last_used_;
    std::atomic<ConnectionState> state_;
    std::atomic<uint64_t> use_count_{0};
    std::atomic<bool> healthy_{true};
};

// Connection pool configuration
struct ConnectionPoolConfig {
    size_t min_connections{2};          // Minimum connections to maintain
    size_t max_connections{10};         // Maximum connections
    size_t max_idle_connections{5};     // Maximum idle connections
    uint32_t connection_timeout_ms{5000}; // Connection creation timeout
    uint32_t max_connection_age_ms{300000}; // Max connection age (5 minutes)
    uint32_t max_idle_time_ms{60000};   // Max idle time before recycling
    uint32_t health_check_interval_ms{30000}; // Health check interval
    uint32_t acquire_timeout_ms{10000}; // Max wait time to acquire connection
    bool validate_on_acquire{true};     // Validate before returning
    bool test_on_return{false};         // Test connection on return
};

// Connection factory interface
template <typename T>
class ConnectionFactory {
public:
    virtual ~ConnectionFactory() = default;

    // Create a new connection
    virtual std::unique_ptr<T> create() = 0;

    // Validate a connection
    virtual bool validate(T* connection) = 0;

    // Reset a connection for reuse
    virtual void reset(T* connection) = 0;

    // Destroy a connection
    virtual void destroy(T* connection) = 0;
};

// RAII connection guard
template <typename T>
class ConnectionGuard;

// Generic connection pool
template <typename T>
class ConnectionPool : public std::enable_shared_from_this<ConnectionPool<T>> {
public:
    using Connection = PooledConnection<T>;
    using ConnectionPtr = typename Connection::Ptr;
    using Factory = ConnectionFactory<T>;

    ConnectionPool(std::shared_ptr<Factory> factory, const ConnectionPoolConfig& config)
        : factory_(std::move(factory)),
          config_(config),
          next_id_(0),
          running_(false) {}

    ~ConnectionPool() {
        shutdown();
    }

    // Initialize the pool
    void initialize() {
        std::lock_guard<std::mutex> lock(mutex_);

        // Create minimum connections
        for (size_t i = 0; i < config_.min_connections; ++i) {
            auto conn = createConnection();
            if (conn) {
                idle_connections_.push_back(conn);
            }
        }

        // Start health check thread
        running_ = true;
        health_check_thread_ = std::make_unique<std::thread>([this]() {
            this->healthCheckLoop();
        });
    }

    // Shutdown the pool
    void shutdown() {
        running_ = false;

        if (health_check_thread_ && health_check_thread_->joinable()) {
            health_check_thread_->join();
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // Clear all connections
        for (auto& conn : idle_connections_) {
            factory_->destroy(conn->get());
        }
        idle_connections_.clear();

        for (auto& [id, conn] : active_connections_) {
            factory_->destroy(conn->get());
        }
        active_connections_.clear();
    }

    // Acquire a connection
    std::optional<ConnectionGuard<T>> acquire() {
        return acquireWithTimeout(config_.acquire_timeout_ms);
    }

    // Acquire with custom timeout
    std::optional<ConnectionGuard<T>> acquireWithTimeout(uint32_t timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(timeout_ms);

        std::unique_lock<std::mutex> lock(mutex_);

        while (true) {
            // Try to get an idle connection
            while (!idle_connections_.empty()) {
                auto conn = idle_connections_.front();
                idle_connections_.pop_front();

                // Validate if required
                if (config_.validate_on_acquire) {
                    if (!validateConnection(conn)) {
                        factory_->destroy(conn->get());
                        continue;
                    }
                }

                // Check age
                if (conn->ageMs() > static_cast<int64_t>(config_.max_connection_age_ms)) {
                    factory_->destroy(conn->get());
                    continue;
                }

                // Mark as in use
                conn->setState(ConnectionState::InUse);
                conn->touch();
                conn->incrementUseCount();
                active_connections_[conn->id()] = conn;

                return ConnectionGuard<T>(
                    conn,
                    this->shared_from_this()
                );
            }

            // Create new connection if below max
            if (totalConnections() < config_.max_connections) {
                lock.unlock();
                auto conn = createConnection();
                lock.lock();

                if (conn) {
                    conn->setState(ConnectionState::InUse);
                    conn->incrementUseCount();
                    active_connections_[conn->id()] = conn;

                    return ConnectionGuard<T>(
                        conn,
                        this->shared_from_this()
                    );
                }
            }

            // Wait for a connection to be released
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return std::nullopt;
            }
        }
    }

    // Release a connection back to the pool
    void release(ConnectionPtr conn) {
        if (!conn) return;

        std::lock_guard<std::mutex> lock(mutex_);

        // Remove from active
        active_connections_.erase(conn->id());

        // Validate if required
        if (config_.test_on_return) {
            if (!validateConnection(conn)) {
                factory_->destroy(conn->get());
                cv_.notify_one();
                return;
            }
        }

        // Reset connection state
        factory_->reset(conn->get());
        conn->setState(ConnectionState::Idle);
        conn->touch();

        // Check if we have too many idle connections
        if (idle_connections_.size() >= config_.max_idle_connections) {
            factory_->destroy(conn->get());
        } else {
            idle_connections_.push_back(conn);
        }

        cv_.notify_one();
    }

    // Invalidate a connection (don't return to pool)
    void invalidate(ConnectionPtr conn) {
        if (!conn) return;

        std::lock_guard<std::mutex> lock(mutex_);
        active_connections_.erase(conn->id());
        factory_->destroy(conn->get());
        cv_.notify_one();
    }

    // Pool statistics
    size_t idleCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return idle_connections_.size();
    }

    size_t activeCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_connections_.size();
    }

    size_t totalConnections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return idle_connections_.size() + active_connections_.size();
    }

    // Update configuration
    void updateConfig(const ConnectionPoolConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
    }

    // Get health check results
    std::vector<HealthCheckResult> getHealthResults() const {
        std::lock_guard<std::mutex> lock(health_mutex_);
        return health_results_;
    }

private:
    ConnectionPtr createConnection() {
        try {
            auto raw_conn = factory_->create();
            if (!raw_conn) {
                return nullptr;
            }

            auto conn = std::make_shared<Connection>(
                std::move(raw_conn),
                next_id_++
            );

            return conn;
        } catch (...) {
            return nullptr;
        }
    }

    bool validateConnection(ConnectionPtr conn) {
        if (!conn || !conn->isHealthy()) {
            return false;
        }

        return factory_->validate(conn->get());
    }

    void healthCheckLoop() {
        while (running_.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.health_check_interval_ms)
            );

            if (!running_.load()) break;

            performHealthCheck();
            recycleStaleConnections();
            ensureMinConnections();
        }
    }

    void performHealthCheck() {
        std::vector<ConnectionPtr> connections_to_check;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            connections_to_check.reserve(idle_connections_.size());
            for (const auto& conn : idle_connections_) {
                connections_to_check.push_back(conn);
            }
        }

        std::vector<HealthCheckResult> results;
        std::vector<ConnectionPtr> invalid_connections;

        for (auto& conn : connections_to_check) {
            HealthCheckResult result;
            result.timestamp = std::chrono::steady_clock::now();

            auto start = std::chrono::steady_clock::now();
            bool valid = factory_->validate(conn->get());
            auto end = std::chrono::steady_clock::now();

            result.latency = std::chrono::duration_cast<std::chrono::microseconds>(
                end - start
            );
            result.healthy = valid;
            conn->setHealthy(valid);

            if (!valid) {
                result.error = "Validation failed";
                invalid_connections.push_back(conn);
            }

            results.push_back(result);
        }

        // Store health results
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            health_results_ = std::move(results);
        }

        // Remove invalid connections
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& invalid : invalid_connections) {
                auto it = std::find(idle_connections_.begin(),
                                   idle_connections_.end(),
                                   invalid);
                if (it != idle_connections_.end()) {
                    idle_connections_.erase(it);
                    factory_->destroy(invalid->get());
                }
            }
        }
    }

    void recycleStaleConnections() {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = idle_connections_.begin();
        while (it != idle_connections_.end()) {
            auto& conn = *it;

            bool should_recycle =
                conn->ageMs() > static_cast<int64_t>(config_.max_connection_age_ms) ||
                conn->idleMs() > static_cast<int64_t>(config_.max_idle_time_ms);

            if (should_recycle) {
                factory_->destroy(conn->get());
                it = idle_connections_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ensureMinConnections() {
        std::lock_guard<std::mutex> lock(mutex_);

        while (totalConnections() < config_.min_connections) {
            auto conn = createConnection();
            if (conn) {
                idle_connections_.push_back(conn);
            } else {
                break;  // Stop if we can't create connections
            }
        }
    }

    std::shared_ptr<Factory> factory_;
    ConnectionPoolConfig config_;

    std::deque<ConnectionPtr> idle_connections_;
    std::unordered_map<size_t, ConnectionPtr> active_connections_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::atomic<size_t> next_id_;
    std::atomic<bool> running_;
    std::unique_ptr<std::thread> health_check_thread_;

    std::vector<HealthCheckResult> health_results_;
    mutable std::mutex health_mutex_;
};

// RAII connection guard
template <typename T>
class ConnectionGuard {
public:
    using Connection = PooledConnection<T>;
    using ConnectionPtr = typename Connection::Ptr;
    using Pool = ConnectionPool<T>;
    using PoolPtr = std::shared_ptr<Pool>;

    ConnectionGuard(ConnectionPtr conn, PoolPtr pool)
        : connection_(conn), pool_(pool), released_(false) {}

    ~ConnectionGuard() {
        release();
    }

    // Move only
    ConnectionGuard(const ConnectionGuard&) = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;

    ConnectionGuard(ConnectionGuard&& other) noexcept
        : connection_(std::move(other.connection_)),
          pool_(std::move(other.pool_)),
          released_(other.released_) {
        other.released_ = true;
    }

    ConnectionGuard& operator=(ConnectionGuard&& other) noexcept {
        if (this != &other) {
            release();
            connection_ = std::move(other.connection_);
            pool_ = std::move(other.pool_);
            released_ = other.released_;
            other.released_ = true;
        }
        return *this;
    }

    // Access underlying connection
    T* get() { return connection_->get(); }
    const T* get() const { return connection_->get(); }

    T* operator->() { return connection_->get(); }
    const T* operator->() const { return connection_->get(); }

    T& operator*() { return *connection_->get(); }
    const T& operator*() const { return *connection_->get(); }

    // Get pooled connection wrapper
    ConnectionPtr connection() { return connection_; }

    // Release back to pool
    void release() {
        if (!released_ && connection_ && pool_) {
            pool_->release(connection_);
            released_ = true;
        }
    }

    // Invalidate connection (don't return to pool)
    void invalidate() {
        if (!released_ && connection_ && pool_) {
            pool_->invalidate(connection_);
            released_ = true;
        }
    }

    // Check if valid
    explicit operator bool() const { return !released_ && connection_; }

private:
    ConnectionPtr connection_;
    PoolPtr pool_;
    bool released_;
};

// Factory for creating connection pools
template <typename T>
std::shared_ptr<ConnectionPool<T>> createConnectionPool(
    std::shared_ptr<ConnectionFactory<T>> factory,
    const ConnectionPoolConfig& config = ConnectionPoolConfig{}) {
    auto pool = std::make_shared<ConnectionPool<T>>(std::move(factory), config);
    pool->initialize();
    return pool;
}

} // namespace network
} // namespace hft
