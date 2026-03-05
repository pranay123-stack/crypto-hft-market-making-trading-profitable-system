#pragma once

/**
 * @file position_manager.hpp
 * @brief Position tracking and PnL calculation for HFT trading system
 *
 * Provides comprehensive position management including:
 * - Position tracking per symbol per exchange
 * - Realized and unrealized PnL calculation
 * - Average entry price tracking
 * - Position reconciliation
 * - Multi-exchange position aggregation
 */

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <atomic>
#include <optional>

#include "core/types.hpp"
#include "oms/order.hpp"

namespace hft::oms {

using namespace hft::core;

/**
 * @brief Position data for a single symbol on a single exchange
 */
struct Position {
    Symbol symbol;
    Exchange exchange{Exchange::Unknown};

    // Position size (positive = long, negative = short)
    double quantity{0.0};

    // Cost basis
    double avgEntryPrice{0.0};
    double totalCost{0.0};  // Signed: positive for longs, negative for shorts

    // Current market price for unrealized PnL
    double markPrice{0.0};

    // PnL tracking
    double realizedPnL{0.0};       // Closed position PnL
    double unrealizedPnL{0.0};     // Open position PnL
    double totalCommissions{0.0};  // Total fees paid

    // Position statistics
    uint64_t totalBuys{0};         // Total buy fills
    uint64_t totalSells{0};        // Total sell fills
    double totalBuyQuantity{0.0};  // Total buy volume
    double totalSellQuantity{0.0}; // Total sell volume
    double totalBuyValue{0.0};     // Total buy notional
    double totalSellValue{0.0};    // Total sell notional

    // Timestamps
    Timestamp openTime;            // When position was opened
    Timestamp lastUpdateTime;      // Last position update

    /**
     * @brief Check if position is flat
     */
    [[nodiscard]] bool isFlat() const noexcept {
        return std::abs(quantity) < 1e-10;
    }

    /**
     * @brief Check if position is long
     */
    [[nodiscard]] bool isLong() const noexcept {
        return quantity > 1e-10;
    }

    /**
     * @brief Check if position is short
     */
    [[nodiscard]] bool isShort() const noexcept {
        return quantity < -1e-10;
    }

    /**
     * @brief Get absolute position size
     */
    [[nodiscard]] double absQuantity() const noexcept {
        return std::abs(quantity);
    }

    /**
     * @brief Get notional value of position
     */
    [[nodiscard]] double notionalValue() const noexcept {
        return absQuantity() * markPrice;
    }

    /**
     * @brief Get total PnL (realized + unrealized)
     */
    [[nodiscard]] double totalPnL() const noexcept {
        return realizedPnL + unrealizedPnL;
    }

    /**
     * @brief Get net PnL (total - commissions)
     */
    [[nodiscard]] double netPnL() const noexcept {
        return totalPnL() - totalCommissions;
    }

    /**
     * @brief Calculate unrealized PnL with given mark price
     */
    [[nodiscard]] double calculateUnrealizedPnL(double price) const noexcept {
        if (isFlat()) return 0.0;
        return quantity * (price - avgEntryPrice);
    }

    /**
     * @brief Convert to string for logging
     */
    [[nodiscard]] std::string toString() const;
};

/**
 * @brief Aggregated position across all exchanges
 */
struct AggregatedPosition {
    Symbol symbol;

    double netQuantity{0.0};          // Sum across all exchanges
    double avgEntryPrice{0.0};        // Weighted average entry
    double realizedPnL{0.0};          // Sum of realized PnL
    double unrealizedPnL{0.0};        // Sum of unrealized PnL
    double totalCommissions{0.0};     // Total commissions

    std::vector<Position> exchangePositions; // Per-exchange breakdown

    [[nodiscard]] bool isFlat() const noexcept {
        return std::abs(netQuantity) < 1e-10;
    }

    [[nodiscard]] double totalPnL() const noexcept {
        return realizedPnL + unrealizedPnL;
    }

    [[nodiscard]] double netPnL() const noexcept {
        return totalPnL() - totalCommissions;
    }
};

/**
 * @brief Position update event
 */
struct PositionUpdate {
    Symbol symbol;
    Exchange exchange{Exchange::Unknown};

    double previousQuantity{0.0};
    double newQuantity{0.0};
    double fillPrice{0.0};
    double fillQuantity{0.0};
    Side fillSide{Side::Buy};

    double realizedPnL{0.0};       // PnL realized from this fill
    double commission{0.0};        // Commission for this fill

    Timestamp timestamp;
};

/**
 * @brief Position manager callback types
 */
using PositionCallback = std::function<void(const PositionUpdate&)>;
using PnLCallback = std::function<void(const Symbol&, double realizedPnL, double unrealizedPnL)>;

/**
 * @brief Position Manager Statistics
 */
struct PositionManagerStats {
    std::atomic<uint64_t> totalFillsProcessed{0};
    std::atomic<uint64_t> positionsOpened{0};
    std::atomic<uint64_t> positionsClosed{0};
    std::atomic<int64_t> totalRealizedPnLCents{0};  // Stored as cents for atomic
    std::atomic<int64_t> totalUnrealizedPnLCents{0};
    std::atomic<int64_t> totalCommissionsCents{0};
};

/**
 * @brief Central Position Manager
 *
 * Thread-safe position management with support for:
 * - Multiple exchanges and symbols
 * - Real-time PnL calculation
 * - Position aggregation
 * - Reconciliation
 */
class PositionManager {
public:
    /**
     * @brief Constructor
     */
    PositionManager();

    /**
     * @brief Destructor
     */
    ~PositionManager();

    // Non-copyable
    PositionManager(const PositionManager&) = delete;
    PositionManager& operator=(const PositionManager&) = delete;

    // ===== Fill Processing =====

    /**
     * @brief Process a fill event
     * @return Realized PnL from this fill (if position reduced)
     */
    double onFill(const Symbol& symbol, Exchange exchange, Side side,
                  Price fillPrice, Quantity fillQty, double commission = 0.0);

    /**
     * @brief Process a fill with raw doubles
     */
    double onFill(const Symbol& symbol, Exchange exchange, Side side,
                  double fillPrice, double fillQty, double commission = 0.0);

    // ===== Price Updates =====

    /**
     * @brief Update mark price for unrealized PnL calculation
     */
    void updateMarkPrice(const Symbol& symbol, Exchange exchange, double price);

    /**
     * @brief Update mark price across all exchanges
     */
    void updateMarkPriceAllExchanges(const Symbol& symbol, double price);

    // ===== Position Queries =====

    /**
     * @brief Get position for symbol on exchange
     */
    [[nodiscard]] std::optional<Position> getPosition(const Symbol& symbol,
                                                       Exchange exchange) const;

    /**
     * @brief Get aggregated position across all exchanges
     */
    [[nodiscard]] AggregatedPosition getAggregatedPosition(const Symbol& symbol) const;

    /**
     * @brief Get all positions on exchange
     */
    [[nodiscard]] std::vector<Position> getPositionsForExchange(Exchange exchange) const;

    /**
     * @brief Get all positions for symbol
     */
    [[nodiscard]] std::vector<Position> getPositionsForSymbol(const Symbol& symbol) const;

    /**
     * @brief Get all non-flat positions
     */
    [[nodiscard]] std::vector<Position> getAllOpenPositions() const;

    /**
     * @brief Get all positions
     */
    [[nodiscard]] std::vector<Position> getAllPositions() const;

    /**
     * @brief Check if position exists
     */
    [[nodiscard]] bool hasPosition(const Symbol& symbol, Exchange exchange) const;

    /**
     * @brief Check if any position exists for symbol
     */
    [[nodiscard]] bool hasAnyPosition(const Symbol& symbol) const;

    // ===== PnL Queries =====

    /**
     * @brief Get total realized PnL across all positions
     */
    [[nodiscard]] double getTotalRealizedPnL() const;

    /**
     * @brief Get total unrealized PnL across all positions
     */
    [[nodiscard]] double getTotalUnrealizedPnL() const;

    /**
     * @brief Get total PnL (realized + unrealized)
     */
    [[nodiscard]] double getTotalPnL() const;

    /**
     * @brief Get total commissions paid
     */
    [[nodiscard]] double getTotalCommissions() const;

    /**
     * @brief Get net PnL (total - commissions)
     */
    [[nodiscard]] double getNetPnL() const;

    /**
     * @brief Get PnL for specific symbol
     */
    [[nodiscard]] double getSymbolPnL(const Symbol& symbol) const;

    /**
     * @brief Get PnL for specific exchange
     */
    [[nodiscard]] double getExchangePnL(Exchange exchange) const;

    // ===== Risk Metrics =====

    /**
     * @brief Get total gross exposure (sum of absolute position values)
     */
    [[nodiscard]] double getGrossExposure() const;

    /**
     * @brief Get net exposure (sum of signed position values)
     */
    [[nodiscard]] double getNetExposure() const;

    /**
     * @brief Get number of open positions
     */
    [[nodiscard]] size_t getOpenPositionCount() const;

    /**
     * @brief Get largest position by notional
     */
    [[nodiscard]] std::optional<Position> getLargestPosition() const;

    // ===== Position Reconciliation =====

    /**
     * @brief Set expected position (from exchange)
     */
    void setExpectedPosition(const Symbol& symbol, Exchange exchange,
                             double quantity, double avgPrice);

    /**
     * @brief Check for position discrepancies
     * @return List of symbols with mismatched positions
     */
    [[nodiscard]] std::vector<Symbol> checkPositionDiscrepancies() const;

    /**
     * @brief Force reconcile position to expected value
     */
    void reconcilePosition(const Symbol& symbol, Exchange exchange);

    // ===== Position Management =====

    /**
     * @brief Close position (set quantity to zero)
     */
    void closePosition(const Symbol& symbol, Exchange exchange);

    /**
     * @brief Flatten all positions
     */
    void flattenAllPositions();

    /**
     * @brief Reset all positions and PnL
     */
    void reset();

    // ===== Callbacks =====

    /**
     * @brief Set callback for position updates
     */
    void setPositionCallback(PositionCallback callback);

    /**
     * @brief Set callback for PnL updates
     */
    void setPnLCallback(PnLCallback callback);

    // ===== Statistics =====

    /**
     * @brief Get position manager statistics
     */
    [[nodiscard]] const PositionManagerStats& getStats() const { return stats_; }

    /**
     * @brief Reset statistics
     */
    void resetStats();

private:
    // Position key: symbol + exchange
    struct PositionKey {
        std::string symbol;
        Exchange exchange;

        bool operator==(const PositionKey& other) const {
            return symbol == other.symbol && exchange == other.exchange;
        }
    };

    struct PositionKeyHash {
        size_t operator()(const PositionKey& key) const {
            return std::hash<std::string>{}(key.symbol) ^
                   (std::hash<int>{}(static_cast<int>(key.exchange)) << 1);
        }
    };

    // Position storage
    mutable std::shared_mutex mutex_;
    std::unordered_map<PositionKey, Position, PositionKeyHash> positions_;

    // Expected positions for reconciliation
    std::unordered_map<PositionKey, Position, PositionKeyHash> expectedPositions_;

    // Callbacks
    PositionCallback positionCallback_;
    PnLCallback pnlCallback_;

    // Statistics
    PositionManagerStats stats_;

    // Helper methods
    Position& getOrCreatePosition(const Symbol& symbol, Exchange exchange);
    void notifyPositionUpdate(const PositionUpdate& update);
    void notifyPnLUpdate(const Symbol& symbol, double realizedPnL, double unrealizedPnL);
    double processPositionChange(Position& pos, Side side, double price, double quantity);
};

} // namespace hft::oms
