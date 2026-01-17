#pragma once

#include "core/types.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <array>
#include <mutex>

namespace hft {

// ============================================================================
// Price Level with Exchange Attribution
// ============================================================================

struct ConsolidatedLevel {
    Price price;
    Quantity total_quantity;

    // Per-exchange breakdown
    struct ExchangeContribution {
        ExchangeId exchange;
        Quantity quantity;
        Timestamp last_update;
    };

    std::vector<ExchangeContribution> contributions;

    void add_contribution(ExchangeId exchange, Quantity qty, Timestamp ts);
    void remove_contribution(ExchangeId exchange);
    [[nodiscard]] Quantity get_exchange_qty(ExchangeId exchange) const;
};

// ============================================================================
// Best Bid/Offer Across Exchanges
// ============================================================================

struct NBBO {  // National Best Bid and Offer
    Price best_bid;
    Price best_ask;
    Quantity best_bid_qty;
    Quantity best_ask_qty;
    ExchangeId best_bid_exchange;
    ExchangeId best_ask_exchange;
    Timestamp timestamp;

    [[nodiscard]] Price spread() const noexcept {
        return best_ask - best_bid;
    }

    [[nodiscard]] double spread_bps() const noexcept {
        Price mid = (best_bid + best_ask) / 2;
        if (mid == 0) return 0.0;
        return 10000.0 * static_cast<double>(spread()) / static_cast<double>(mid);
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return best_bid > 0 && best_ask > 0 && best_bid < best_ask;
    }
};

// ============================================================================
// Per-Exchange Order Book
// ============================================================================

struct ExchangeBook {
    ExchangeId exchange;
    std::map<Price, Quantity, std::greater<>> bids;  // Descending
    std::map<Price, Quantity, std::less<>> asks;     // Ascending
    Timestamp last_update = 0;
    SequenceNum sequence = 0;

    [[nodiscard]] Price best_bid() const {
        return bids.empty() ? 0 : bids.begin()->first;
    }

    [[nodiscard]] Price best_ask() const {
        return asks.empty() ? 0 : asks.begin()->first;
    }

    [[nodiscard]] Quantity best_bid_qty() const {
        return bids.empty() ? 0 : bids.begin()->second;
    }

    [[nodiscard]] Quantity best_ask_qty() const {
        return asks.empty() ? 0 : asks.begin()->second;
    }
};

// ============================================================================
// Consolidated Order Book - Aggregates Multiple Exchanges
// ============================================================================

class ConsolidatedBook {
public:
    static constexpr size_t MAX_DEPTH = 50;
    static constexpr size_t MAX_EXCHANGES = 8;

    explicit ConsolidatedBook(const Symbol& symbol);
    ~ConsolidatedBook() = default;

    // Per-exchange updates
    void update_bid(ExchangeId exchange, Price price, Quantity qty);
    void update_ask(ExchangeId exchange, Price price, Quantity qty);
    void apply_snapshot(ExchangeId exchange,
                       const std::vector<std::pair<Price, Quantity>>& bids,
                       const std::vector<std::pair<Price, Quantity>>& asks);
    void clear_exchange(ExchangeId exchange);

    // Consolidated view
    [[nodiscard]] NBBO get_nbbo() const;
    [[nodiscard]] const ConsolidatedLevel* get_consolidated_bid(size_t depth) const;
    [[nodiscard]] const ConsolidatedLevel* get_consolidated_ask(size_t depth) const;

    // Per-exchange view
    [[nodiscard]] const ExchangeBook* get_exchange_book(ExchangeId exchange) const;
    [[nodiscard]] Price get_exchange_bid(ExchangeId exchange) const;
    [[nodiscard]] Price get_exchange_ask(ExchangeId exchange) const;

    // Arbitrage detection
    [[nodiscard]] bool has_arbitrage_opportunity() const;
    [[nodiscard]] ArbitrageOpportunity find_arbitrage() const;

    // Cross-exchange spread
    [[nodiscard]] double cross_exchange_spread_bps() const;

    // Volume-weighted prices
    [[nodiscard]] Price consolidated_vwap_bid(Quantity qty) const;
    [[nodiscard]] Price consolidated_vwap_ask(Quantity qty) const;

    // Imbalance across exchanges
    [[nodiscard]] double total_book_imbalance(size_t levels = 5) const;
    [[nodiscard]] std::unordered_map<ExchangeId, double> per_exchange_imbalance() const;

    // State
    [[nodiscard]] const Symbol& symbol() const noexcept { return symbol_; }
    [[nodiscard]] Timestamp last_update() const noexcept { return last_update_; }
    [[nodiscard]] size_t active_exchange_count() const;

private:
    void rebuild_consolidated_book();
    void update_nbbo();

    Symbol symbol_;

    // Per-exchange books
    std::array<ExchangeBook, MAX_EXCHANGES> exchange_books_;
    std::array<bool, MAX_EXCHANGES> exchange_active_{};

    // Consolidated view (cached)
    std::vector<ConsolidatedLevel> consolidated_bids_;
    std::vector<ConsolidatedLevel> consolidated_asks_;
    NBBO nbbo_;

    bool consolidated_dirty_ = true;
    Timestamp last_update_ = 0;
    mutable std::mutex mutex_;
};

// ============================================================================
// Consolidated Book Manager - Multiple Symbols
// ============================================================================

class ConsolidatedBookManager {
public:
    ConsolidatedBookManager() = default;

    ConsolidatedBook& get_or_create(const Symbol& symbol);
    ConsolidatedBook* get(const Symbol& symbol);
    const ConsolidatedBook* get(const Symbol& symbol) const;

    void update(ExchangeId exchange, const Symbol& symbol,
                const Tick& tick);

    [[nodiscard]] std::vector<ArbitrageOpportunity> find_all_arbitrage() const;

    void clear();
    [[nodiscard]] size_t size() const { return books_.size(); }

private:
    std::unordered_map<std::string, std::unique_ptr<ConsolidatedBook>> books_;
    mutable std::mutex mutex_;
};

} // namespace hft
