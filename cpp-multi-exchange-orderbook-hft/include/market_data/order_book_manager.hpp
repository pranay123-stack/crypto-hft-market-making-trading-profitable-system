#pragma once

#include "exchange/exchange_base.hpp"
#include "core/lock_free_queue.hpp"

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <map>
#include <vector>
#include <chrono>
#include <thread>

namespace hft {
namespace market_data {

using namespace exchange;

// ============================================================================
// Enums
// ============================================================================

enum class BookUpdateType : uint8_t {
    Snapshot,       // Full orderbook snapshot
    Delta,          // Incremental update
    Trade           // Trade occurred
};

enum class BookState : uint8_t {
    Uninitialized,  // No data received yet
    Initializing,   // Waiting for snapshot
    Ready,          // Normal operation
    Stale,          // No updates for too long
    GapDetected,    // Sequence gap, needs resync
    Error           // Error state
};

// ============================================================================
// Structures
// ============================================================================

struct OrderBookDelta {
    std::string symbol;
    std::string exchange;
    uint64_t first_update_id{0};    // For sequence validation
    uint64_t last_update_id{0};     // Last update ID in this delta
    uint64_t exchange_timestamp{0}; // Exchange timestamp (ms)
    uint64_t local_timestamp{0};    // Local receive timestamp (ns)

    std::vector<PriceLevel> bid_updates;  // qty=0 means remove level
    std::vector<PriceLevel> ask_updates;  // qty=0 means remove level

    bool is_snapshot{false};
};

struct BookMetadata {
    BookState state{BookState::Uninitialized};
    uint64_t last_update_id{0};
    uint64_t last_update_time{0};       // Nanoseconds
    uint64_t snapshot_time{0};
    uint64_t gap_count{0};
    uint64_t update_count{0};
    uint64_t snapshot_count{0};
};

struct GapEvent {
    std::string symbol;
    std::string exchange;
    uint64_t expected_id;
    uint64_t received_id;
    uint64_t timestamp;
};

// ============================================================================
// Configuration
// ============================================================================

struct OrderBookManagerConfig {
    size_t max_depth{50};                           // Max price levels to maintain
    uint64_t staleness_threshold_us{5000000};       // 5 seconds default
    uint64_t gap_recovery_timeout_us{10000000};     // 10 seconds
    bool enable_staleness_check{true};
    bool enable_statistics{true};
    size_t event_queue_size{65536};
};

// ============================================================================
// Callbacks
// ============================================================================

using OnBookUpdateCallback = std::function<void(const OrderBook&)>;
using OnGapDetectedCallback = std::function<void(const GapEvent&)>;
using OnStateChangeCallback = std::function<void(const std::string& symbol,
                                                  const std::string& exchange,
                                                  BookState old_state,
                                                  BookState new_state)>;
using OnStalenessCallback = std::function<void(const std::string& symbol,
                                                const std::string& exchange,
                                                uint64_t last_update_age_us)>;

// ============================================================================
// ManagedOrderBook - Internal book with price level management
// ============================================================================

class ManagedOrderBook {
public:
    ManagedOrderBook(const std::string& symbol, const std::string& exchange, size_t max_depth);

    // Apply updates
    void applySnapshot(const OrderBookDelta& snapshot);
    bool applyDelta(const OrderBookDelta& delta);  // Returns false on sequence gap

    // Accessors
    OrderBook getSnapshot() const;
    BookMetadata getMetadata() const;
    BookState getState() const { return state_.load(std::memory_order_acquire); }
    uint64_t getLastUpdateId() const { return last_update_id_.load(std::memory_order_acquire); }
    uint64_t getLastUpdateTime() const { return last_update_time_.load(std::memory_order_acquire); }

    // State management
    void setState(BookState state);
    void markStale();
    void reset();

    // Statistics
    uint64_t getGapCount() const { return gap_count_.load(std::memory_order_relaxed); }
    uint64_t getUpdateCount() const { return update_count_.load(std::memory_order_relaxed); }

private:
    void applyBidUpdates(const std::vector<PriceLevel>& updates);
    void applyAskUpdates(const std::vector<PriceLevel>& updates);
    void trimToMaxDepth();

    std::string symbol_;
    std::string exchange_;
    size_t max_depth_;

    // Price levels: map for O(log n) insert/update/delete
    // Bids: sorted descending (use std::greater)
    // Asks: sorted ascending (use std::less)
    std::map<double, PriceLevel, std::greater<double>> bids_;
    std::map<double, PriceLevel, std::less<double>> asks_;

    // Atomic state
    std::atomic<BookState> state_{BookState::Uninitialized};
    std::atomic<uint64_t> last_update_id_{0};
    std::atomic<uint64_t> last_update_time_{0};
    std::atomic<uint64_t> exchange_timestamp_{0};

    // Statistics
    std::atomic<uint64_t> gap_count_{0};
    std::atomic<uint64_t> update_count_{0};
    std::atomic<uint64_t> snapshot_count_{0};

    // Thread safety
    mutable std::shared_mutex mutex_;
};

// ============================================================================
// OrderBookManager - Central manager for all order books
// ============================================================================

class OrderBookManager {
public:
    explicit OrderBookManager(const OrderBookManagerConfig& config = {});
    ~OrderBookManager();

    // Lifecycle
    void start();
    void stop();

    // Configuration
    void setConfig(const OrderBookManagerConfig& config);

    // Book management
    void registerSymbol(const std::string& symbol, const std::string& exchange);
    void unregisterSymbol(const std::string& symbol, const std::string& exchange);

    // Apply updates (thread-safe)
    void applySnapshot(const OrderBookDelta& snapshot);
    bool applyDelta(const OrderBookDelta& delta);  // Returns false on gap

    // Accessors
    std::optional<OrderBook> getBook(const std::string& symbol, const std::string& exchange) const;
    std::optional<BookMetadata> getMetadata(const std::string& symbol, const std::string& exchange) const;
    BookState getState(const std::string& symbol, const std::string& exchange) const;

    // Staleness check
    bool isStale(const std::string& symbol, const std::string& exchange) const;
    std::vector<std::pair<std::string, std::string>> getStaleBooks() const;

    // Request snapshot (callback to exchange connector)
    using SnapshotRequestCallback = std::function<void(const std::string& symbol, const std::string& exchange)>;
    void setSnapshotRequestCallback(SnapshotRequestCallback callback);
    void requestSnapshot(const std::string& symbol, const std::string& exchange);

    // Event callbacks
    void setOnBookUpdate(OnBookUpdateCallback callback);
    void setOnGapDetected(OnGapDetectedCallback callback);
    void setOnStateChange(OnStateChangeCallback callback);
    void setOnStaleness(OnStalenessCallback callback);

    // Statistics
    struct Statistics {
        uint64_t total_updates{0};
        uint64_t total_snapshots{0};
        uint64_t total_gaps{0};
        uint64_t total_stale_events{0};
        size_t active_books{0};
        uint64_t avg_update_latency_ns{0};
    };
    Statistics getStatistics() const;

private:
    std::string makeKey(const std::string& symbol, const std::string& exchange) const;
    void stalenessCheckLoop();
    void notifyBookUpdate(const OrderBook& book);
    void notifyGap(const GapEvent& gap);
    void notifyStateChange(const std::string& symbol, const std::string& exchange,
                          BookState old_state, BookState new_state);
    void notifyStaleness(const std::string& symbol, const std::string& exchange, uint64_t age_us);

    OrderBookManagerConfig config_;

    // Books: key = "exchange:symbol"
    std::unordered_map<std::string, std::unique_ptr<ManagedOrderBook>> books_;
    mutable std::shared_mutex books_mutex_;

    // Callbacks
    OnBookUpdateCallback on_book_update_;
    OnGapDetectedCallback on_gap_detected_;
    OnStateChangeCallback on_state_change_;
    OnStalenessCallback on_staleness_;
    SnapshotRequestCallback snapshot_request_callback_;
    std::mutex callback_mutex_;

    // Background thread for staleness checks
    std::thread staleness_thread_;
    std::atomic<bool> running_{false};

    // Statistics
    std::atomic<uint64_t> total_updates_{0};
    std::atomic<uint64_t> total_snapshots_{0};
    std::atomic<uint64_t> total_gaps_{0};
    std::atomic<uint64_t> total_stale_events_{0};
};

// ============================================================================
// Inline Implementations
// ============================================================================

inline std::string OrderBookManager::makeKey(const std::string& symbol,
                                              const std::string& exchange) const {
    return exchange + ":" + symbol;
}

} // namespace market_data
} // namespace hft
