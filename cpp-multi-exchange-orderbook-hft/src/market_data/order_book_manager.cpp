#include "market_data/order_book_manager.hpp"

#include <algorithm>
#include <chrono>

namespace hft {
namespace market_data {

// ============================================================================
// ManagedOrderBook Implementation
// ============================================================================

ManagedOrderBook::ManagedOrderBook(const std::string& symbol,
                                   const std::string& exchange,
                                   size_t max_depth)
    : symbol_(symbol)
    , exchange_(exchange)
    , max_depth_(max_depth) {
}

void ManagedOrderBook::applySnapshot(const OrderBookDelta& snapshot) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Clear existing data
    bids_.clear();
    asks_.clear();

    // Apply bid levels
    for (const auto& level : snapshot.bid_updates) {
        if (level.quantity > 0) {
            bids_[level.price] = level;
        }
    }

    // Apply ask levels
    for (const auto& level : snapshot.ask_updates) {
        if (level.quantity > 0) {
            asks_[level.price] = level;
        }
    }

    trimToMaxDepth();

    // Update metadata
    last_update_id_.store(snapshot.last_update_id, std::memory_order_release);
    last_update_time_.store(snapshot.local_timestamp, std::memory_order_release);
    exchange_timestamp_.store(snapshot.exchange_timestamp, std::memory_order_release);
    snapshot_count_.fetch_add(1, std::memory_order_relaxed);

    state_.store(BookState::Ready, std::memory_order_release);
}

bool ManagedOrderBook::applyDelta(const OrderBookDelta& delta) {
    // Check sequence
    uint64_t expected = last_update_id_.load(std::memory_order_acquire) + 1;

    // Gap detection: if first_update_id > expected, we have a gap
    if (delta.first_update_id > expected && state_.load(std::memory_order_acquire) == BookState::Ready) {
        gap_count_.fetch_add(1, std::memory_order_relaxed);
        state_.store(BookState::GapDetected, std::memory_order_release);
        return false;
    }

    // Skip if this delta is older than what we have
    if (delta.last_update_id <= last_update_id_.load(std::memory_order_acquire)) {
        return true;  // Not an error, just stale data
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Apply updates
    applyBidUpdates(delta.bid_updates);
    applyAskUpdates(delta.ask_updates);
    trimToMaxDepth();

    // Update metadata
    last_update_id_.store(delta.last_update_id, std::memory_order_release);
    last_update_time_.store(delta.local_timestamp, std::memory_order_release);
    exchange_timestamp_.store(delta.exchange_timestamp, std::memory_order_release);
    update_count_.fetch_add(1, std::memory_order_relaxed);

    if (state_.load(std::memory_order_acquire) != BookState::Ready) {
        state_.store(BookState::Ready, std::memory_order_release);
    }

    return true;
}

void ManagedOrderBook::applyBidUpdates(const std::vector<PriceLevel>& updates) {
    for (const auto& level : updates) {
        if (level.quantity == 0) {
            // Remove level
            bids_.erase(level.price);
        } else {
            // Insert or update
            bids_[level.price] = level;
        }
    }
}

void ManagedOrderBook::applyAskUpdates(const std::vector<PriceLevel>& updates) {
    for (const auto& level : updates) {
        if (level.quantity == 0) {
            // Remove level
            asks_.erase(level.price);
        } else {
            // Insert or update
            asks_[level.price] = level;
        }
    }
}

void ManagedOrderBook::trimToMaxDepth() {
    // Trim bids (already sorted descending)
    while (bids_.size() > max_depth_) {
        bids_.erase(std::prev(bids_.end()));
    }

    // Trim asks (already sorted ascending)
    while (asks_.size() > max_depth_) {
        asks_.erase(std::prev(asks_.end()));
    }
}

OrderBook ManagedOrderBook::getSnapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    OrderBook book;
    book.symbol = symbol_;
    book.exchange = exchange_;
    book.sequence = last_update_id_.load(std::memory_order_acquire);
    book.timestamp = exchange_timestamp_.load(std::memory_order_acquire);
    book.local_timestamp = last_update_time_.load(std::memory_order_acquire);

    book.bids.reserve(bids_.size());
    for (const auto& [price, level] : bids_) {
        book.bids.push_back(level);
    }

    book.asks.reserve(asks_.size());
    for (const auto& [price, level] : asks_) {
        book.asks.push_back(level);
    }

    return book;
}

BookMetadata ManagedOrderBook::getMetadata() const {
    BookMetadata meta;
    meta.state = state_.load(std::memory_order_acquire);
    meta.last_update_id = last_update_id_.load(std::memory_order_acquire);
    meta.last_update_time = last_update_time_.load(std::memory_order_acquire);
    meta.gap_count = gap_count_.load(std::memory_order_relaxed);
    meta.update_count = update_count_.load(std::memory_order_relaxed);
    meta.snapshot_count = snapshot_count_.load(std::memory_order_relaxed);
    return meta;
}

void ManagedOrderBook::setState(BookState state) {
    state_.store(state, std::memory_order_release);
}

void ManagedOrderBook::markStale() {
    state_.store(BookState::Stale, std::memory_order_release);
}

void ManagedOrderBook::reset() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    bids_.clear();
    asks_.clear();
    state_.store(BookState::Uninitialized, std::memory_order_release);
    last_update_id_.store(0, std::memory_order_release);
    last_update_time_.store(0, std::memory_order_release);
}

// ============================================================================
// OrderBookManager Implementation
// ============================================================================

OrderBookManager::OrderBookManager(const OrderBookManagerConfig& config)
    : config_(config) {
}

OrderBookManager::~OrderBookManager() {
    stop();
}

void OrderBookManager::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(true, std::memory_order_release);

    if (config_.enable_staleness_check) {
        staleness_thread_ = std::thread(&OrderBookManager::stalenessCheckLoop, this);
    }
}

void OrderBookManager::stop() {
    running_.store(false, std::memory_order_release);

    if (staleness_thread_.joinable()) {
        staleness_thread_.join();
    }
}

void OrderBookManager::setConfig(const OrderBookManagerConfig& config) {
    config_ = config;
}

void OrderBookManager::registerSymbol(const std::string& symbol, const std::string& exchange) {
    std::unique_lock<std::shared_mutex> lock(books_mutex_);

    std::string key = makeKey(symbol, exchange);
    if (books_.find(key) == books_.end()) {
        books_[key] = std::make_unique<ManagedOrderBook>(symbol, exchange, config_.max_depth);
    }
}

void OrderBookManager::unregisterSymbol(const std::string& symbol, const std::string& exchange) {
    std::unique_lock<std::shared_mutex> lock(books_mutex_);
    books_.erase(makeKey(symbol, exchange));
}

void OrderBookManager::applySnapshot(const OrderBookDelta& snapshot) {
    std::string key = makeKey(snapshot.symbol, snapshot.exchange);

    std::shared_lock<std::shared_mutex> lock(books_mutex_);
    auto it = books_.find(key);
    if (it == books_.end()) {
        return;
    }

    BookState old_state = it->second->getState();
    it->second->applySnapshot(snapshot);
    BookState new_state = it->second->getState();

    total_snapshots_.fetch_add(1, std::memory_order_relaxed);

    if (old_state != new_state) {
        notifyStateChange(snapshot.symbol, snapshot.exchange, old_state, new_state);
    }

    notifyBookUpdate(it->second->getSnapshot());
}

bool OrderBookManager::applyDelta(const OrderBookDelta& delta) {
    std::string key = makeKey(delta.symbol, delta.exchange);

    std::shared_lock<std::shared_mutex> lock(books_mutex_);
    auto it = books_.find(key);
    if (it == books_.end()) {
        return false;
    }

    BookState old_state = it->second->getState();
    bool success = it->second->applyDelta(delta);
    BookState new_state = it->second->getState();

    total_updates_.fetch_add(1, std::memory_order_relaxed);

    if (!success) {
        // Gap detected
        total_gaps_.fetch_add(1, std::memory_order_relaxed);

        GapEvent gap;
        gap.symbol = delta.symbol;
        gap.exchange = delta.exchange;
        gap.expected_id = it->second->getLastUpdateId() + 1;
        gap.received_id = delta.first_update_id;
        gap.timestamp = delta.local_timestamp;

        notifyGap(gap);

        // Request fresh snapshot
        requestSnapshot(delta.symbol, delta.exchange);
    }

    if (old_state != new_state) {
        notifyStateChange(delta.symbol, delta.exchange, old_state, new_state);
    }

    if (success) {
        notifyBookUpdate(it->second->getSnapshot());
    }

    return success;
}

std::optional<OrderBook> OrderBookManager::getBook(const std::string& symbol,
                                                    const std::string& exchange) const {
    std::shared_lock<std::shared_mutex> lock(books_mutex_);
    auto it = books_.find(makeKey(symbol, exchange));
    if (it == books_.end()) {
        return std::nullopt;
    }
    return it->second->getSnapshot();
}

std::optional<BookMetadata> OrderBookManager::getMetadata(const std::string& symbol,
                                                           const std::string& exchange) const {
    std::shared_lock<std::shared_mutex> lock(books_mutex_);
    auto it = books_.find(makeKey(symbol, exchange));
    if (it == books_.end()) {
        return std::nullopt;
    }
    return it->second->getMetadata();
}

BookState OrderBookManager::getState(const std::string& symbol, const std::string& exchange) const {
    std::shared_lock<std::shared_mutex> lock(books_mutex_);
    auto it = books_.find(makeKey(symbol, exchange));
    if (it == books_.end()) {
        return BookState::Uninitialized;
    }
    return it->second->getState();
}

bool OrderBookManager::isStale(const std::string& symbol, const std::string& exchange) const {
    std::shared_lock<std::shared_mutex> lock(books_mutex_);
    auto it = books_.find(makeKey(symbol, exchange));
    if (it == books_.end()) {
        return true;
    }

    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    auto last_update = it->second->getLastUpdateTime();
    uint64_t age_ns = now - last_update;

    return age_ns > (config_.staleness_threshold_us * 1000);
}

std::vector<std::pair<std::string, std::string>> OrderBookManager::getStaleBooks() const {
    std::vector<std::pair<std::string, std::string>> stale;

    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    uint64_t threshold_ns = config_.staleness_threshold_us * 1000;

    std::shared_lock<std::shared_mutex> lock(books_mutex_);
    for (const auto& [key, book] : books_) {
        auto last_update = book->getLastUpdateTime();
        if (last_update > 0 && (now - last_update) > threshold_ns) {
            // Parse key back to exchange:symbol
            auto pos = key.find(':');
            if (pos != std::string::npos) {
                stale.emplace_back(key.substr(pos + 1), key.substr(0, pos));
            }
        }
    }

    return stale;
}

void OrderBookManager::setSnapshotRequestCallback(SnapshotRequestCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    snapshot_request_callback_ = std::move(callback);
}

void OrderBookManager::requestSnapshot(const std::string& symbol, const std::string& exchange) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (snapshot_request_callback_) {
        snapshot_request_callback_(symbol, exchange);
    }
}

void OrderBookManager::setOnBookUpdate(OnBookUpdateCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_book_update_ = std::move(callback);
}

void OrderBookManager::setOnGapDetected(OnGapDetectedCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_gap_detected_ = std::move(callback);
}

void OrderBookManager::setOnStateChange(OnStateChangeCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_state_change_ = std::move(callback);
}

void OrderBookManager::setOnStaleness(OnStalenessCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_staleness_ = std::move(callback);
}

OrderBookManager::Statistics OrderBookManager::getStatistics() const {
    Statistics stats;
    stats.total_updates = total_updates_.load(std::memory_order_relaxed);
    stats.total_snapshots = total_snapshots_.load(std::memory_order_relaxed);
    stats.total_gaps = total_gaps_.load(std::memory_order_relaxed);
    stats.total_stale_events = total_stale_events_.load(std::memory_order_relaxed);

    std::shared_lock<std::shared_mutex> lock(books_mutex_);
    stats.active_books = books_.size();

    return stats;
}

void OrderBookManager::stalenessCheckLoop() {
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        uint64_t threshold_ns = config_.staleness_threshold_us * 1000;

        std::shared_lock<std::shared_mutex> lock(books_mutex_);
        for (const auto& [key, book] : books_) {
            auto state = book->getState();
            if (state != BookState::Ready) {
                continue;
            }

            auto last_update = book->getLastUpdateTime();
            if (last_update == 0) {
                continue;
            }

            uint64_t age_ns = now - last_update;
            if (age_ns > threshold_ns) {
                book->markStale();
                total_stale_events_.fetch_add(1, std::memory_order_relaxed);

                // Parse key
                auto pos = key.find(':');
                if (pos != std::string::npos) {
                    std::string exchange = key.substr(0, pos);
                    std::string symbol = key.substr(pos + 1);

                    notifyStaleness(symbol, exchange, age_ns / 1000);
                    notifyStateChange(symbol, exchange, BookState::Ready, BookState::Stale);

                    // Request new snapshot
                    lock.unlock();
                    requestSnapshot(symbol, exchange);
                    lock.lock();
                }
            }
        }
    }
}

void OrderBookManager::notifyBookUpdate(const OrderBook& book) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (on_book_update_) {
        on_book_update_(book);
    }
}

void OrderBookManager::notifyGap(const GapEvent& gap) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (on_gap_detected_) {
        on_gap_detected_(gap);
    }
}

void OrderBookManager::notifyStateChange(const std::string& symbol, const std::string& exchange,
                                          BookState old_state, BookState new_state) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (on_state_change_) {
        on_state_change_(symbol, exchange, old_state, new_state);
    }
}

void OrderBookManager::notifyStaleness(const std::string& symbol, const std::string& exchange,
                                        uint64_t age_us) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (on_staleness_) {
        on_staleness_(symbol, exchange, age_us);
    }
}

} // namespace market_data
} // namespace hft
