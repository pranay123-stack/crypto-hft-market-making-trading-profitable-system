#pragma once

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <cstdint>

namespace hft {
namespace core {

// ============================================================================
// Forward Declarations
// ============================================================================

class ICheckpointProvider;

// ============================================================================
// Structures
// ============================================================================

struct OrderState {
    std::string order_id;
    std::string client_order_id;
    std::string exchange_order_id;
    std::string exchange;
    std::string symbol;
    int side;           // 0=Buy, 1=Sell
    int type;           // 0=Market, 1=Limit, etc.
    int status;         // OrderStatus enum
    double price;
    double quantity;
    double filled_qty;
    double avg_fill_price;
    uint64_t create_time;
    uint64_t update_time;
};

struct PositionState {
    std::string symbol;
    std::string exchange;
    double quantity;
    double avg_entry_price;
    double realized_pnl;
    double unrealized_pnl;
    uint64_t last_update_time;
};

struct Checkpoint {
    // Header
    static constexpr uint32_t MAGIC = 0x48465443;  // "HFTC"
    static constexpr uint16_t VERSION = 1;

    uint64_t checkpoint_id{0};
    uint64_t created_at{0};              // Nanoseconds since epoch
    uint16_t version{VERSION};
    std::string system_version;

    // Trading state
    std::vector<OrderState> open_orders;
    std::vector<PositionState> positions;

    // PnL state
    double realized_pnl{0.0};
    double unrealized_pnl{0.0};
    double daily_pnl{0.0};
    double high_water_mark{0.0};

    // Sequence tracking (for order book recovery)
    std::unordered_map<std::string, uint64_t> last_sequences;  // exchange -> sequence

    // Risk state
    double current_exposure{0.0};
    double max_drawdown{0.0};
    int orders_today{0};
    int fills_today{0};

    // Custom provider data
    std::unordered_map<std::string, std::vector<uint8_t>> provider_data;

    // Serialization
    std::vector<uint8_t> serialize() const;
    static Checkpoint deserialize(const std::vector<uint8_t>& data);
    static bool validate(const std::vector<uint8_t>& data);

    // CRC32 calculation
    static uint32_t calculateCrc32(const uint8_t* data, size_t length);
};

// ============================================================================
// Configuration
// ============================================================================

struct CheckpointConfig {
    std::string checkpoint_dir{"./checkpoints"};
    std::chrono::seconds checkpoint_interval{60};
    size_t max_checkpoints{10};          // Keep last N checkpoints
    bool enable_compression{false};       // Gzip compression
    bool sync_to_disk{true};             // fsync after write
    bool auto_cleanup{true};             // Auto-delete old checkpoints
    std::string file_prefix{"checkpoint"};
};

// ============================================================================
// Checkpoint Provider Interface
// ============================================================================

class ICheckpointProvider {
public:
    virtual ~ICheckpointProvider() = default;

    // Get current state as bytes
    virtual std::vector<uint8_t> getState() = 0;

    // Restore state from bytes
    virtual bool setState(const std::vector<uint8_t>& state) = 0;

    // Provider name (for identification)
    virtual std::string getName() const = 0;

    // Priority for restore order (lower = first)
    virtual int getRestorePriority() const { return 100; }
};

// ============================================================================
// Checkpoint Manager
// ============================================================================

class CheckpointManager {
public:
    explicit CheckpointManager(const CheckpointConfig& config = {});
    ~CheckpointManager();

    // Lifecycle
    void start();
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // Configuration
    void setConfig(const CheckpointConfig& config);
    const CheckpointConfig& getConfig() const { return config_; }

    // Provider registration
    void registerProvider(const std::string& name, std::shared_ptr<ICheckpointProvider> provider);
    void unregisterProvider(const std::string& name);

    // Manual checkpoint
    bool createCheckpoint();
    uint64_t getLastCheckpointId() const { return last_checkpoint_id_.load(std::memory_order_acquire); }

    // Recovery
    bool hasCheckpoint() const;
    std::optional<Checkpoint> loadLatestCheckpoint() const;
    std::optional<Checkpoint> loadCheckpoint(uint64_t checkpoint_id) const;
    std::vector<uint64_t> listCheckpointIds() const;

    // Restore from checkpoint
    bool restoreFromCheckpoint(const Checkpoint& checkpoint);
    bool restoreFromLatest();

    // Direct state access (for manual checkpoint building)
    void setOpenOrders(const std::vector<OrderState>& orders);
    void setPositions(const std::vector<PositionState>& positions);
    void setPnLState(double realized, double unrealized, double daily, double hwm);
    void setRiskState(double exposure, double drawdown, int orders, int fills);
    void setSequence(const std::string& exchange, uint64_t sequence);

    // Callbacks
    using OnCheckpointCreated = std::function<void(uint64_t checkpoint_id)>;
    using OnCheckpointRestored = std::function<void(uint64_t checkpoint_id)>;
    using OnCheckpointError = std::function<void(const std::string& error)>;

    void setOnCheckpointCreated(OnCheckpointCreated callback);
    void setOnCheckpointRestored(OnCheckpointRestored callback);
    void setOnCheckpointError(OnCheckpointError callback);

    // Statistics
    struct Statistics {
        uint64_t checkpoints_created{0};
        uint64_t checkpoints_restored{0};
        uint64_t checkpoint_errors{0};
        uint64_t last_checkpoint_time{0};
        uint64_t last_checkpoint_size{0};
        uint64_t last_checkpoint_duration_us{0};
    };
    Statistics getStatistics() const;

    // Cleanup
    void cleanupOldCheckpoints();

private:
    void checkpointLoop();
    std::string getCheckpointPath(uint64_t checkpoint_id) const;
    bool writeCheckpoint(const Checkpoint& checkpoint);
    std::optional<Checkpoint> readCheckpoint(const std::string& path) const;
    void notifyCreated(uint64_t id);
    void notifyRestored(uint64_t id);
    void notifyError(const std::string& error);

    Checkpoint buildCheckpoint();

    CheckpointConfig config_;

    // Providers
    std::unordered_map<std::string, std::shared_ptr<ICheckpointProvider>> providers_;
    mutable std::shared_mutex providers_mutex_;

    // Current state (set manually or via providers)
    std::vector<OrderState> current_orders_;
    std::vector<PositionState> current_positions_;
    double realized_pnl_{0.0};
    double unrealized_pnl_{0.0};
    double daily_pnl_{0.0};
    double high_water_mark_{0.0};
    double current_exposure_{0.0};
    double max_drawdown_{0.0};
    int orders_today_{0};
    int fills_today_{0};
    std::unordered_map<std::string, uint64_t> sequences_;
    mutable std::mutex state_mutex_;

    // Background thread
    std::thread checkpoint_thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> last_checkpoint_id_{0};

    // Callbacks
    OnCheckpointCreated on_created_;
    OnCheckpointRestored on_restored_;
    OnCheckpointError on_error_;
    std::mutex callback_mutex_;

    // Statistics
    std::atomic<uint64_t> checkpoints_created_{0};
    std::atomic<uint64_t> checkpoints_restored_{0};
    std::atomic<uint64_t> checkpoint_errors_{0};
    std::atomic<uint64_t> last_checkpoint_time_{0};
    std::atomic<uint64_t> last_checkpoint_size_{0};
    std::atomic<uint64_t> last_checkpoint_duration_us_{0};
};

} // namespace core
} // namespace hft
