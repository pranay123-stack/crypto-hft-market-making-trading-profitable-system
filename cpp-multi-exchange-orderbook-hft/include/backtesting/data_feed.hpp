#pragma once

/**
 * @file data_feed.hpp
 * @brief Historical market data feed for backtesting
 *
 * Provides efficient loading and streaming of historical market data
 * including order book snapshots, trade data, and OHLCV bars.
 * Supports CSV and binary formats with memory-efficient streaming.
 */

#include "core/types.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <fstream>
#include <unordered_map>
#include <optional>
#include <filesystem>
#include <span>
#include <mutex>
#include <atomic>
#include <queue>

namespace hft::backtesting {

// ============================================================================
// Market Data Structures
// ============================================================================

/**
 * @brief Single price level in order book
 */
struct PriceLevel {
    core::Price price;
    core::Quantity quantity;
    uint32_t order_count{0};  // Number of orders at this level

    [[nodiscard]] constexpr bool operator<(const PriceLevel& other) const noexcept {
        return price < other.price;
    }

    [[nodiscard]] constexpr bool operator>(const PriceLevel& other) const noexcept {
        return price > other.price;
    }
};

/**
 * @brief Order book snapshot
 */
struct OrderBookSnapshot {
    core::Timestamp timestamp;
    core::Exchange exchange;
    core::Symbol symbol;
    uint64_t sequence_number{0};

    static constexpr size_t MAX_DEPTH = 50;
    std::array<PriceLevel, MAX_DEPTH> bids{};
    std::array<PriceLevel, MAX_DEPTH> asks{};
    uint8_t bid_count{0};
    uint8_t ask_count{0};

    [[nodiscard]] core::Price best_bid() const noexcept {
        return bid_count > 0 ? bids[0].price : core::Price{0};
    }

    [[nodiscard]] core::Price best_ask() const noexcept {
        return ask_count > 0 ? asks[0].price : core::Price{0};
    }

    [[nodiscard]] core::Price mid_price() const noexcept {
        if (bid_count == 0 || ask_count == 0) return core::Price{0};
        return core::Price{(bids[0].price.value + asks[0].price.value) / 2};
    }

    [[nodiscard]] core::Price spread() const noexcept {
        if (bid_count == 0 || ask_count == 0) return core::Price{0};
        return asks[0].price - bids[0].price;
    }

    [[nodiscard]] double spread_bps() const noexcept {
        auto mid = mid_price();
        if (mid.is_zero()) return 0.0;
        return (spread().to_double() / mid.to_double()) * 10000.0;
    }
};

/**
 * @brief Order book delta update
 */
struct OrderBookUpdate {
    enum class UpdateType : uint8_t {
        Set = 0,     // Set price level
        Delete = 1,  // Delete price level
        Snapshot = 2 // Full snapshot follows
    };

    core::Timestamp timestamp;
    core::Exchange exchange;
    core::Symbol symbol;
    uint64_t sequence_number{0};

    UpdateType type{UpdateType::Set};
    core::Side side;
    core::Price price;
    core::Quantity quantity;
    uint32_t order_count{0};
};

/**
 * @brief Trade tick data
 */
struct Trade {
    core::Timestamp timestamp;
    core::Exchange exchange;
    core::Symbol symbol;
    core::TradeId trade_id;

    core::Price price;
    core::Quantity quantity;
    core::Side aggressor_side;  // Side of the taker

    [[nodiscard]] double notional() const noexcept {
        return price.to_double() * quantity.to_double();
    }
};

/**
 * @brief OHLCV bar data
 */
struct OHLCVBar {
    core::Timestamp timestamp;      // Bar open time
    core::Timestamp close_time;     // Bar close time
    core::Exchange exchange;
    core::Symbol symbol;

    core::Price open;
    core::Price high;
    core::Price low;
    core::Price close;
    core::Quantity volume;
    core::Quantity quote_volume;    // Volume in quote currency
    uint32_t trade_count{0};

    [[nodiscard]] double vwap() const noexcept {
        if (volume.is_zero()) return close.to_double();
        return quote_volume.to_double() / volume.to_double();
    }

    [[nodiscard]] bool is_bullish() const noexcept {
        return close >= open;
    }

    [[nodiscard]] double body_size() const noexcept {
        return std::abs(close.to_double() - open.to_double());
    }

    [[nodiscard]] double range() const noexcept {
        return high.to_double() - low.to_double();
    }
};

// ============================================================================
// Data Event (Union type for all market data)
// ============================================================================

/**
 * @brief Event type enumeration
 */
enum class DataEventType : uint8_t {
    OrderBookSnapshot = 0,
    OrderBookUpdate = 1,
    Trade = 2,
    OHLCVBar = 3
};

/**
 * @brief Unified market data event
 */
struct DataEvent {
    DataEventType type;
    core::Timestamp timestamp;
    core::Exchange exchange;
    core::Symbol symbol;

    union {
        struct {
            const OrderBookSnapshot* snapshot;
        } order_book;
        struct {
            const OrderBookUpdate* update;
        } book_update;
        struct {
            const Trade* trade;
        } trade;
        struct {
            const OHLCVBar* bar;
        } ohlcv;
    } data;

    DataEvent() : type(DataEventType::Trade), timestamp{}, exchange{}, symbol{}, data{} {
        data.trade.trade = nullptr;
    }
};

// ============================================================================
// Data Feed Configuration
// ============================================================================

/**
 * @brief Data file format enumeration
 */
enum class DataFormat : uint8_t {
    CSV = 0,
    Binary = 1,
    Parquet = 2,  // Future support
    HDF5 = 3      // Future support
};

/**
 * @brief Data feed configuration
 */
struct DataFeedConfig {
    std::vector<std::filesystem::path> data_paths;  // Directories or files
    DataFormat format{DataFormat::CSV};

    core::Timestamp start_time;
    core::Timestamp end_time;

    std::vector<core::Exchange> exchanges;
    std::vector<core::Symbol> symbols;

    // Performance settings
    size_t buffer_size{1024 * 1024};        // 1MB read buffer
    size_t prefetch_events{10000};          // Events to prefetch
    bool memory_map_files{true};            // Use mmap for large files
    size_t max_memory_usage{1024 * 1024 * 1024};  // 1GB max memory

    // Data filtering
    bool include_order_book_snapshots{true};
    bool include_order_book_updates{true};
    bool include_trades{true};
    bool include_ohlcv{true};
    uint32_t order_book_depth{20};          // Levels to load

    // CSV parsing settings
    char csv_delimiter{','};
    bool csv_has_header{true};
    std::string timestamp_format{"%Y-%m-%d %H:%M:%S"};
};

// ============================================================================
// Data Parsers
// ============================================================================

/**
 * @brief CSV parser for market data
 */
class CSVParser {
public:
    explicit CSVParser(char delimiter = ',');

    // Parse individual record types
    [[nodiscard]] std::optional<Trade> parseTrade(std::string_view line) const;
    [[nodiscard]] std::optional<OHLCVBar> parseOHLCV(std::string_view line) const;
    [[nodiscard]] std::optional<OrderBookSnapshot> parseOrderBookSnapshot(
        std::string_view line) const;
    [[nodiscard]] std::optional<OrderBookUpdate> parseOrderBookUpdate(
        std::string_view line) const;

    // Batch parsing
    [[nodiscard]] std::vector<Trade> parseTrades(std::istream& input, size_t max_count = 0) const;
    [[nodiscard]] std::vector<OHLCVBar> parseOHLCVBars(std::istream& input, size_t max_count = 0) const;

    // Auto-detect format and parse
    [[nodiscard]] DataEventType detectFormat(std::string_view header) const;

    void setTimestampFormat(const std::string& format) { timestamp_format_ = format; }
    void setExchange(core::Exchange exchange) { default_exchange_ = exchange; }
    void setSymbol(const core::Symbol& symbol) { default_symbol_ = symbol; }

private:
    char delimiter_;
    std::string timestamp_format_;
    core::Exchange default_exchange_{core::Exchange::Unknown};
    core::Symbol default_symbol_;

    [[nodiscard]] std::vector<std::string_view> splitLine(std::string_view line) const;
    [[nodiscard]] core::Timestamp parseTimestamp(std::string_view ts) const;
    [[nodiscard]] core::Price parsePrice(std::string_view price) const;
    [[nodiscard]] core::Quantity parseQuantity(std::string_view qty) const;
};

/**
 * @brief Binary data parser for high-performance loading
 */
class BinaryParser {
public:
    // Binary format magic numbers
    static constexpr uint32_t TRADE_MAGIC = 0x54524144;      // "TRAD"
    static constexpr uint32_t OHLCV_MAGIC = 0x4F484C56;      // "OHLV"
    static constexpr uint32_t ORDERBOOK_MAGIC = 0x4F424B53;  // "OBKS"

    // Binary record sizes
    static constexpr size_t TRADE_RECORD_SIZE = 48;
    static constexpr size_t OHLCV_RECORD_SIZE = 72;

    [[nodiscard]] std::optional<Trade> parseTrade(const uint8_t* data, size_t size) const;
    [[nodiscard]] std::optional<OHLCVBar> parseOHLCV(const uint8_t* data, size_t size) const;
    [[nodiscard]] std::optional<OrderBookSnapshot> parseOrderBookSnapshot(
        const uint8_t* data, size_t size) const;

    // Batch loading from memory-mapped file
    [[nodiscard]] std::vector<Trade> loadTrades(const uint8_t* data, size_t size) const;
    [[nodiscard]] std::vector<OHLCVBar> loadOHLCVBars(const uint8_t* data, size_t size) const;

    // Write binary format
    [[nodiscard]] std::vector<uint8_t> serializeTrade(const Trade& trade) const;
    [[nodiscard]] std::vector<uint8_t> serializeOHLCV(const OHLCVBar& bar) const;
    [[nodiscard]] std::vector<uint8_t> serializeOrderBookSnapshot(
        const OrderBookSnapshot& snapshot) const;
};

// ============================================================================
// Data File Handler
// ============================================================================

/**
 * @brief Memory-mapped file handler for efficient data access
 */
class MemoryMappedFile {
public:
    MemoryMappedFile() = default;
    ~MemoryMappedFile();

    // Non-copyable
    MemoryMappedFile(const MemoryMappedFile&) = delete;
    MemoryMappedFile& operator=(const MemoryMappedFile&) = delete;

    // Movable
    MemoryMappedFile(MemoryMappedFile&& other) noexcept;
    MemoryMappedFile& operator=(MemoryMappedFile&& other) noexcept;

    [[nodiscard]] bool open(const std::filesystem::path& path);
    void close();

    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }

private:
    uint8_t* data_{nullptr};
    size_t size_{0};
    int fd_{-1};
};

// ============================================================================
// Data Stream (Iterator-based access)
// ============================================================================

/**
 * @brief Streaming data iterator for memory-efficient access
 */
template<typename T>
class DataStream {
public:
    class Iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        Iterator() = default;
        explicit Iterator(DataStream* stream, bool end = false);

        reference operator*() const { return current_; }
        pointer operator->() const { return &current_; }

        Iterator& operator++();
        Iterator operator++(int);

        [[nodiscard]] bool operator==(const Iterator& other) const;
        [[nodiscard]] bool operator!=(const Iterator& other) const;

    private:
        DataStream* stream_{nullptr};
        T current_{};
        bool at_end_{true};
    };

    virtual ~DataStream() = default;

    [[nodiscard]] virtual bool hasNext() const = 0;
    [[nodiscard]] virtual std::optional<T> next() = 0;
    virtual void reset() = 0;

    [[nodiscard]] Iterator begin() { return Iterator(this); }
    [[nodiscard]] Iterator end() { return Iterator(this, true); }
};

// ============================================================================
// Main Data Feed Class
// ============================================================================

/**
 * @brief Main data feed class for backtesting
 *
 * Provides unified access to historical market data from multiple sources.
 * Supports time-synchronized iteration across multiple exchanges and symbols.
 */
class DataFeed {
public:
    using EventCallback = std::function<void(const DataEvent&)>;

    explicit DataFeed(const DataFeedConfig& config);
    ~DataFeed();

    // Non-copyable
    DataFeed(const DataFeed&) = delete;
    DataFeed& operator=(const DataFeed&) = delete;

    // Initialize and load data
    [[nodiscard]] bool initialize();
    void shutdown();

    // Event iteration
    [[nodiscard]] bool hasNext() const;
    [[nodiscard]] std::optional<DataEvent> next();
    void reset();

    // Callback-based iteration
    void forEach(EventCallback callback);

    // Time-based access
    [[nodiscard]] std::optional<DataEvent> seekTo(core::Timestamp timestamp);
    [[nodiscard]] core::Timestamp currentTime() const noexcept { return current_time_; }
    [[nodiscard]] core::Timestamp startTime() const noexcept { return config_.start_time; }
    [[nodiscard]] core::Timestamp endTime() const noexcept { return config_.end_time; }

    // Specific data access
    [[nodiscard]] std::optional<OrderBookSnapshot> getOrderBook(
        core::Exchange exchange, const core::Symbol& symbol) const;
    [[nodiscard]] std::vector<Trade> getTrades(
        core::Exchange exchange, const core::Symbol& symbol,
        core::Timestamp start, core::Timestamp end) const;
    [[nodiscard]] std::vector<OHLCVBar> getOHLCV(
        core::Exchange exchange, const core::Symbol& symbol,
        core::Timestamp start, core::Timestamp end) const;

    // Statistics
    [[nodiscard]] uint64_t totalEvents() const noexcept { return total_events_; }
    [[nodiscard]] uint64_t eventsProcessed() const noexcept { return events_processed_; }
    [[nodiscard]] double progress() const noexcept;

    // Data info
    [[nodiscard]] const std::vector<core::Exchange>& exchanges() const { return loaded_exchanges_; }
    [[nodiscard]] const std::vector<core::Symbol>& symbols() const { return loaded_symbols_; }
    [[nodiscard]] size_t memoryUsage() const noexcept { return memory_usage_; }

private:
    // Data loading methods
    [[nodiscard]] bool loadCSVFile(const std::filesystem::path& path);
    [[nodiscard]] bool loadBinaryFile(const std::filesystem::path& path);
    [[nodiscard]] bool scanDirectory(const std::filesystem::path& dir);

    // Event queue management
    void refillEventQueue();
    void sortEventQueue();

    // Internal data structures
    struct DataSource {
        std::filesystem::path path;
        DataFormat format;
        DataEventType event_type;
        core::Exchange exchange;
        core::Symbol symbol;
        size_t current_offset{0};
        std::unique_ptr<MemoryMappedFile> mmap;
        std::unique_ptr<std::ifstream> stream;
    };

    DataFeedConfig config_;
    std::vector<DataSource> sources_;

    // Loaded data storage
    std::vector<Trade> trades_;
    std::vector<OHLCVBar> ohlcv_bars_;
    std::vector<OrderBookSnapshot> order_book_snapshots_;
    std::vector<OrderBookUpdate> order_book_updates_;

    // Current order book state per exchange/symbol
    std::unordered_map<std::string, OrderBookSnapshot> current_order_books_;

    // Event queue for time-synchronized iteration
    struct TimestampedEvent {
        DataEvent event;
        size_t source_index;

        [[nodiscard]] bool operator>(const TimestampedEvent& other) const {
            return event.timestamp > other.event.timestamp;
        }
    };
    std::priority_queue<TimestampedEvent, std::vector<TimestampedEvent>,
                        std::greater<TimestampedEvent>> event_queue_;

    // Iteration state
    core::Timestamp current_time_;
    size_t current_trade_idx_{0};
    size_t current_ohlcv_idx_{0};
    size_t current_snapshot_idx_{0};
    size_t current_update_idx_{0};

    // Statistics
    uint64_t total_events_{0};
    uint64_t events_processed_{0};
    size_t memory_usage_{0};

    // Loaded data info
    std::vector<core::Exchange> loaded_exchanges_;
    std::vector<core::Symbol> loaded_symbols_;

    // Thread safety
    mutable std::mutex mutex_;
    std::atomic<bool> initialized_{false};
};

// ============================================================================
// Multi-Exchange Data Synchronizer
// ============================================================================

/**
 * @brief Synchronizes data across multiple exchanges
 *
 * Ensures events are delivered in chronological order across all exchanges,
 * handling clock skew and latency differences.
 */
class MultiExchangeDataSynchronizer {
public:
    struct SyncConfig {
        uint64_t max_clock_skew_us{1000};      // Maximum allowed clock skew
        uint64_t sync_window_us{100};           // Synchronization window
        bool adjust_timestamps{false};          // Apply clock correction
    };

    explicit MultiExchangeDataSynchronizer(const SyncConfig& config);

    // Add data feeds for each exchange
    void addFeed(core::Exchange exchange, std::shared_ptr<DataFeed> feed);

    // Synchronized iteration
    [[nodiscard]] bool hasNext() const;
    [[nodiscard]] std::optional<DataEvent> next();
    void reset();

    // Get current synchronized time
    [[nodiscard]] core::Timestamp currentTime() const noexcept { return current_time_; }

private:
    struct ExchangeFeed {
        core::Exchange exchange;
        std::shared_ptr<DataFeed> feed;
        std::optional<DataEvent> buffered_event;
        int64_t clock_offset{0};  // Estimated clock offset in microseconds
    };

    SyncConfig config_;
    std::vector<ExchangeFeed> feeds_;
    core::Timestamp current_time_;

    void estimateClockOffsets();
};

// ============================================================================
// Data Feed Factory
// ============================================================================

/**
 * @brief Factory for creating data feeds
 */
class DataFeedFactory {
public:
    // Create from configuration
    [[nodiscard]] static std::unique_ptr<DataFeed> create(const DataFeedConfig& config);

    // Create from single file
    [[nodiscard]] static std::unique_ptr<DataFeed> fromFile(
        const std::filesystem::path& path,
        core::Exchange exchange,
        const core::Symbol& symbol);

    // Create from directory
    [[nodiscard]] static std::unique_ptr<DataFeed> fromDirectory(
        const std::filesystem::path& dir,
        core::Timestamp start,
        core::Timestamp end);

    // Create synthetic data for testing
    [[nodiscard]] static std::unique_ptr<DataFeed> createSynthetic(
        core::Exchange exchange,
        const core::Symbol& symbol,
        core::Timestamp start,
        core::Timestamp end,
        uint64_t events_per_second = 1000);
};

}  // namespace hft::backtesting
