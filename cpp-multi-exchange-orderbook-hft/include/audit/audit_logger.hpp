#pragma once

#include "core/lock_free_queue.hpp"

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <optional>

namespace hft {
namespace audit {

// ============================================================================
// Enums
// ============================================================================

enum class AuditEventType : uint8_t {
    // Order lifecycle
    ORDER_SENT = 0,
    ORDER_ACK = 1,
    ORDER_REJECT = 2,
    ORDER_FILL = 3,
    ORDER_PARTIAL_FILL = 4,
    ORDER_CANCEL_SENT = 5,
    ORDER_CANCEL_ACK = 6,
    ORDER_CANCEL_REJECT = 7,
    ORDER_MODIFY_SENT = 8,
    ORDER_MODIFY_ACK = 9,
    ORDER_EXPIRED = 10,

    // Risk events
    RISK_REJECT = 20,
    RISK_WARNING = 21,
    CIRCUIT_BREAKER_TRIGGERED = 22,
    CIRCUIT_BREAKER_RESET = 23,

    // Position events
    POSITION_OPENED = 30,
    POSITION_CLOSED = 31,
    POSITION_UPDATED = 32,

    // Connectivity events
    CONNECTION_UP = 40,
    CONNECTION_DOWN = 41,
    CONNECTION_ERROR = 42,
    RECONNECT_ATTEMPT = 43,

    // Market data events
    MARKET_DATA_GAP = 50,
    MARKET_DATA_STALE = 51,
    ORDERBOOK_SNAPSHOT = 52,

    // System events
    SYSTEM_START = 60,
    SYSTEM_STOP = 61,
    SYSTEM_ERROR = 62,
    CHECKPOINT_CREATED = 63,
    CHECKPOINT_RESTORED = 64,

    // Strategy events
    STRATEGY_SIGNAL = 70,
    STRATEGY_ERROR = 71,

    // Custom
    CUSTOM = 255
};

enum class OrderSideAudit : uint8_t {
    BUY = 0,
    SELL = 1,
    UNKNOWN = 255
};

enum class OrderTypeAudit : uint8_t {
    MARKET = 0,
    LIMIT = 1,
    STOP_MARKET = 2,
    STOP_LIMIT = 3,
    UNKNOWN = 255
};

// ============================================================================
// Structures
// ============================================================================

struct AuditEntry {
    // Header (fixed size for fast binary reading)
    static constexpr uint32_t MAGIC = 0x41554454;  // "AUDT"
    static constexpr uint16_t VERSION = 1;

    uint64_t sequence_id{0};           // Monotonic sequence for ordering
    uint64_t timestamp_ns{0};          // Nanosecond precision local time
    AuditEventType event_type{AuditEventType::CUSTOM};

    // Identifiers (variable length in serialization)
    std::string order_id;
    std::string client_order_id;
    std::string exchange_order_id;
    std::string exchange;
    std::string symbol;

    // Order details
    OrderSideAudit side{OrderSideAudit::UNKNOWN};
    OrderTypeAudit type{OrderTypeAudit::UNKNOWN};
    double price{0.0};
    double quantity{0.0};
    double filled_qty{0.0};
    double avg_fill_price{0.0};
    double commission{0.0};

    // Timestamps for latency analysis
    uint64_t exchange_timestamp_ns{0};
    uint64_t local_send_time_ns{0};
    uint64_t local_receive_time_ns{0};

    // Response/reason
    std::string reason;
    std::string raw_request;
    std::string raw_response;

    // Extra metadata
    std::string metadata;

    // Serialization
    std::vector<uint8_t> toBinary() const;
    std::string toJson() const;
    static AuditEntry fromBinary(const uint8_t* data, size_t length, size_t& bytes_read);
};

// ============================================================================
// Configuration
// ============================================================================

struct AuditConfig {
    std::string output_dir{"./audit_logs"};
    std::string file_prefix{"audit"};
    size_t rotation_size_bytes{100 * 1024 * 1024};  // 100MB
    int retention_days{90};                          // Keep for 90 days
    bool enable_compression{false};                  // Gzip old files
    bool enable_json{false};                         // Also write JSON (slower)
    bool sync_to_disk{false};                        // fsync after each write
    size_t queue_size{65536};                        // Async queue size
    size_t batch_size{100};                          // Batch writes for efficiency
};

// ============================================================================
// AuditLogger - Singleton
// ============================================================================

class AuditLogger {
public:
    static AuditLogger& instance() {
        static AuditLogger instance;
        return instance;
    }

    // Disable copy
    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    // Configuration
    void setConfig(const AuditConfig& config);
    const AuditConfig& getConfig() const { return config_; }

    // Lifecycle
    void start();
    void stop();
    void flush();  // Force flush to disk
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // ========================================================================
    // Order logging methods
    // ========================================================================

    void logOrderSent(const std::string& order_id,
                      const std::string& client_order_id,
                      const std::string& exchange,
                      const std::string& symbol,
                      OrderSideAudit side,
                      OrderTypeAudit type,
                      double price,
                      double quantity,
                      const std::string& raw_request = "");

    void logOrderAck(const std::string& order_id,
                     const std::string& exchange_order_id,
                     const std::string& exchange,
                     const std::string& symbol,
                     uint64_t exchange_timestamp_ns,
                     const std::string& raw_response = "");

    void logOrderReject(const std::string& order_id,
                        const std::string& exchange,
                        const std::string& symbol,
                        const std::string& reason,
                        const std::string& raw_response = "");

    void logOrderFill(const std::string& order_id,
                      const std::string& exchange_order_id,
                      const std::string& exchange,
                      const std::string& symbol,
                      OrderSideAudit side,
                      double fill_price,
                      double fill_qty,
                      double total_filled,
                      double commission,
                      bool is_partial,
                      const std::string& raw_message = "");

    void logOrderCancel(const std::string& order_id,
                        const std::string& exchange,
                        const std::string& symbol,
                        const std::string& raw_request = "");

    void logOrderCancelAck(const std::string& order_id,
                           const std::string& exchange,
                           const std::string& symbol,
                           const std::string& raw_response = "");

    // ========================================================================
    // Risk logging methods
    // ========================================================================

    void logRiskReject(const std::string& order_id,
                       const std::string& exchange,
                       const std::string& symbol,
                       const std::string& reason);

    void logCircuitBreaker(bool triggered,
                           const std::string& reason,
                           const std::string& details = "");

    // ========================================================================
    // Position logging methods
    // ========================================================================

    void logPositionChange(const std::string& exchange,
                           const std::string& symbol,
                           double old_qty,
                           double new_qty,
                           double avg_entry_price,
                           double realized_pnl);

    // ========================================================================
    // Connectivity logging methods
    // ========================================================================

    void logConnectionUp(const std::string& exchange,
                         const std::string& connection_type);

    void logConnectionDown(const std::string& exchange,
                           const std::string& connection_type,
                           const std::string& reason = "");

    void logReconnectAttempt(const std::string& exchange,
                             int attempt_number);

    // ========================================================================
    // Market data logging methods
    // ========================================================================

    void logMarketDataGap(const std::string& exchange,
                          const std::string& symbol,
                          uint64_t expected_seq,
                          uint64_t received_seq);

    void logMarketDataStale(const std::string& exchange,
                            const std::string& symbol,
                            uint64_t staleness_ms);

    // ========================================================================
    // System logging methods
    // ========================================================================

    void logSystemStart(const std::string& version,
                        const std::string& config_summary = "");

    void logSystemStop(const std::string& reason = "normal");

    void logSystemError(const std::string& error,
                        const std::string& details = "");

    void logCheckpoint(bool created,
                       uint64_t checkpoint_id);

    // ========================================================================
    // Strategy logging methods
    // ========================================================================

    void logStrategySignal(const std::string& strategy_name,
                           const std::string& symbol,
                           const std::string& signal_type,
                           double confidence,
                           const std::string& details = "");

    // ========================================================================
    // Generic logging
    // ========================================================================

    void log(AuditEntry entry);

    // ========================================================================
    // Statistics
    // ========================================================================

    struct Statistics {
        uint64_t entries_logged{0};
        uint64_t entries_written{0};
        uint64_t bytes_written{0};
        uint64_t files_rotated{0};
        uint64_t queue_overflows{0};
        uint64_t current_file_size{0};
    };
    Statistics getStatistics() const;

private:
    AuditLogger();
    ~AuditLogger();

    void writerLoop();
    void rotateFile();
    void cleanupOldFiles();
    std::string getCurrentFilePath() const;
    uint64_t getNextSequenceId();
    uint64_t getCurrentTimestampNs() const;

    AuditConfig config_;

    // Async queue
    static constexpr size_t QUEUE_CAPACITY = 65536;
    core::SPSCQueue<AuditEntry, QUEUE_CAPACITY> queue_;

    // Writer thread
    std::thread writer_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> flush_requested_{false};

    // File management
    std::ofstream binary_file_;
    std::ofstream json_file_;
    std::string current_file_path_;
    std::atomic<uint64_t> current_file_size_{0};
    std::mutex file_mutex_;

    // Sequence tracking
    std::atomic<uint64_t> sequence_counter_{0};

    // Statistics
    std::atomic<uint64_t> entries_logged_{0};
    std::atomic<uint64_t> entries_written_{0};
    std::atomic<uint64_t> bytes_written_{0};
    std::atomic<uint64_t> files_rotated_{0};
    std::atomic<uint64_t> queue_overflows_{0};
};

// ============================================================================
// AuditReader - For querying audit logs
// ============================================================================

class AuditReader {
public:
    explicit AuditReader(const std::string& audit_dir);

    // List available log files
    std::vector<std::string> listLogFiles() const;

    // Query methods
    std::vector<AuditEntry> queryByOrderId(const std::string& order_id);
    std::vector<AuditEntry> queryByTimeRange(uint64_t start_ns, uint64_t end_ns);
    std::vector<AuditEntry> queryByEventType(AuditEventType type);
    std::vector<AuditEntry> queryByExchange(const std::string& exchange);
    std::vector<AuditEntry> queryBySymbol(const std::string& symbol);

    // Read all entries from a file
    std::vector<AuditEntry> readFile(const std::string& path);

    // Export
    void exportToCsv(const std::vector<AuditEntry>& entries,
                     const std::string& output_path);

private:
    std::string audit_dir_;
};

// ============================================================================
// Helper Macros
// ============================================================================

#define AUDIT_ORDER_SENT(order_id, client_order_id, exchange, symbol, side, type, price, qty) \
    ::hft::audit::AuditLogger::instance().logOrderSent( \
        order_id, client_order_id, exchange, symbol, side, type, price, qty)

#define AUDIT_ORDER_FILL(order_id, exch_order_id, exchange, symbol, side, price, qty, total, comm, partial) \
    ::hft::audit::AuditLogger::instance().logOrderFill( \
        order_id, exch_order_id, exchange, symbol, side, price, qty, total, comm, partial)

#define AUDIT_RISK_REJECT(order_id, exchange, symbol, reason) \
    ::hft::audit::AuditLogger::instance().logRiskReject(order_id, exchange, symbol, reason)

#define AUDIT_CIRCUIT_BREAKER(triggered, reason) \
    ::hft::audit::AuditLogger::instance().logCircuitBreaker(triggered, reason)

} // namespace audit
} // namespace hft
