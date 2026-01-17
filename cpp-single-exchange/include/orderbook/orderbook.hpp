#pragma once

#include "core/types.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <array>
#include <optional>

namespace hft {

// ============================================================================
// Price Level - Aggregated orders at a single price
// ============================================================================

struct PriceLevel {
    Price price;
    Quantity quantity;
    uint32_t order_count;
    Timestamp last_update;

    PriceLevel() : price(0), quantity(0), order_count(0), last_update(0) {}
    PriceLevel(Price p, Quantity q, uint32_t c = 1)
        : price(p), quantity(q), order_count(c), last_update(0) {}
};

// ============================================================================
// Order Book - L2 Market Data with L3 support
// ============================================================================

class OrderBook {
public:
    static constexpr size_t MAX_DEPTH = 100;

    explicit OrderBook(const Symbol& symbol);
    ~OrderBook() = default;

    // Non-copyable for performance
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // L2 Updates (price level aggregated)
    void update_bid(Price price, Quantity quantity);
    void update_ask(Price price, Quantity quantity);
    void clear_bids();
    void clear_asks();

    // L3 Updates (individual orders)
    void add_order(const Order& order);
    void modify_order(OrderId id, Quantity new_qty);
    void remove_order(OrderId id);

    // Snapshot
    void apply_snapshot(const std::vector<PriceLevel>& bids,
                       const std::vector<PriceLevel>& asks);

    // Market Data Queries
    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<Quantity> best_bid_qty() const noexcept;
    [[nodiscard]] std::optional<Quantity> best_ask_qty() const noexcept;

    [[nodiscard]] Price mid_price() const noexcept;
    [[nodiscard]] Price spread() const noexcept;
    [[nodiscard]] double spread_bps() const noexcept;

    // Depth queries
    [[nodiscard]] const PriceLevel* get_bid_level(size_t depth) const noexcept;
    [[nodiscard]] const PriceLevel* get_ask_level(size_t depth) const noexcept;
    [[nodiscard]] size_t bid_depth() const noexcept;
    [[nodiscard]] size_t ask_depth() const noexcept;

    // Volume-weighted prices
    [[nodiscard]] Price vwap_bid(Quantity qty) const;
    [[nodiscard]] Price vwap_ask(Quantity qty) const;

    // Imbalance metrics
    [[nodiscard]] double book_imbalance() const noexcept;
    [[nodiscard]] double book_imbalance(size_t levels) const noexcept;

    // State
    [[nodiscard]] const Symbol& symbol() const noexcept { return symbol_; }
    [[nodiscard]] Timestamp last_update() const noexcept { return last_update_; }
    [[nodiscard]] SequenceNum sequence() const noexcept { return sequence_; }
    [[nodiscard]] bool is_valid() const noexcept;

    void set_sequence(SequenceNum seq) noexcept { sequence_ = seq; }
    void set_timestamp(Timestamp ts) noexcept { last_update_ = ts; }

private:
    void rebuild_bid_cache();
    void rebuild_ask_cache();

    Symbol symbol_;

    // Price -> PriceLevel maps (sorted)
    std::map<Price, PriceLevel, std::greater<>> bids_;  // Descending
    std::map<Price, PriceLevel, std::less<>> asks_;     // Ascending

    // L3: OrderId -> Order mapping
    std::unordered_map<OrderId, Order> orders_;

    // Cached top-of-book for fast access
    std::array<PriceLevel, MAX_DEPTH> bid_cache_;
    std::array<PriceLevel, MAX_DEPTH> ask_cache_;
    size_t bid_cache_size_ = 0;
    size_t ask_cache_size_ = 0;
    bool bid_cache_dirty_ = true;
    bool ask_cache_dirty_ = true;

    Timestamp last_update_ = 0;
    SequenceNum sequence_ = 0;
};

// ============================================================================
// Order Book Manager - Multiple symbols
// ============================================================================

class OrderBookManager {
public:
    OrderBookManager() = default;

    OrderBook& get_or_create(const Symbol& symbol);
    OrderBook* get(const Symbol& symbol);
    const OrderBook* get(const Symbol& symbol) const;

    void remove(const Symbol& symbol);
    void clear();

    [[nodiscard]] size_t size() const noexcept { return books_.size(); }

private:
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
};

} // namespace hft
