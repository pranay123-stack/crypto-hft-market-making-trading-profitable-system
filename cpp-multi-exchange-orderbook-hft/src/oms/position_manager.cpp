/**
 * @file position_manager.cpp
 * @brief Implementation of Position Manager
 */

#include "oms/position_manager.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace hft::oms {

std::string Position::toString() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8);
    oss << "Position{"
        << "symbol=" << symbol.view()
        << ", exchange=" << exchange_to_string(exchange)
        << ", qty=" << quantity
        << ", avgEntry=" << avgEntryPrice
        << ", markPrice=" << markPrice
        << ", realizedPnL=" << realizedPnL
        << ", unrealizedPnL=" << unrealizedPnL
        << ", commissions=" << totalCommissions
        << ", netPnL=" << netPnL()
        << "}";
    return oss.str();
}

// ============================================================================
// PositionManager Implementation
// ============================================================================

PositionManager::PositionManager() {
    LOG_INFO("PositionManager initialized");
}

PositionManager::~PositionManager() {
    auto positions = getAllOpenPositions();
    LOG_INFO("PositionManager shutting down - {} open positions, total PnL: {}",
             positions.size(), getTotalPnL());
}

double PositionManager::onFill(const Symbol& symbol, Exchange exchange, Side side,
                                Price fillPrice, Quantity fillQty, double commission) {
    return onFill(symbol, exchange, side,
                  fillPrice.to_double(), fillQty.to_double(), commission);
}

double PositionManager::onFill(const Symbol& symbol, Exchange exchange, Side side,
                                double fillPrice, double fillQty, double commission) {
    if (fillQty <= 0 || fillPrice <= 0) {
        LOG_WARN("Invalid fill: price={}, qty={}", fillPrice, fillQty);
        return 0.0;
    }

    std::unique_lock lock(mutex_);

    Position& pos = getOrCreatePosition(symbol, exchange);
    double previousQty = pos.quantity;

    // Process the fill
    double realizedPnL = processPositionChange(pos, side, fillPrice, fillQty);

    // Update statistics
    pos.totalCommissions += commission;
    pos.lastUpdateTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    if (side == Side::Buy) {
        pos.totalBuys++;
        pos.totalBuyQuantity += fillQty;
        pos.totalBuyValue += fillPrice * fillQty;
    } else {
        pos.totalSells++;
        pos.totalSellQuantity += fillQty;
        pos.totalSellValue += fillPrice * fillQty;
    }

    // Update global stats
    stats_.totalFillsProcessed.fetch_add(1, std::memory_order_relaxed);
    if (realizedPnL != 0.0) {
        int64_t pnlCents = static_cast<int64_t>(realizedPnL * 100);
        stats_.totalRealizedPnLCents.fetch_add(pnlCents, std::memory_order_relaxed);
    }
    if (commission > 0) {
        int64_t commCents = static_cast<int64_t>(commission * 100);
        stats_.totalCommissionsCents.fetch_add(commCents, std::memory_order_relaxed);
    }

    // Track position opens/closes
    if (pos.isFlat() && !std::abs(previousQty) < 1e-10) {
        stats_.positionsClosed.fetch_add(1, std::memory_order_relaxed);
    } else if (!pos.isFlat() && std::abs(previousQty) < 1e-10) {
        stats_.positionsOpened.fetch_add(1, std::memory_order_relaxed);
    }

    // Update unrealized PnL
    if (!pos.isFlat() && pos.markPrice > 0) {
        pos.unrealizedPnL = pos.calculateUnrealizedPnL(pos.markPrice);
    } else {
        pos.unrealizedPnL = 0.0;
    }

    // Create position update event
    PositionUpdate update;
    update.symbol = symbol;
    update.exchange = exchange;
    update.previousQuantity = previousQty;
    update.newQuantity = pos.quantity;
    update.fillPrice = fillPrice;
    update.fillQuantity = fillQty;
    update.fillSide = side;
    update.realizedPnL = realizedPnL;
    update.commission = commission;
    update.timestamp = pos.lastUpdateTime;

    lock.unlock();

    LOG_DEBUG("Fill processed: {} {} {} @ {} on {} - realized PnL: {}",
              symbol.view(), side_to_string(side), fillQty, fillPrice,
              exchange_to_string(exchange), realizedPnL);

    notifyPositionUpdate(update);
    notifyPnLUpdate(symbol, realizedPnL, pos.unrealizedPnL);

    return realizedPnL;
}

void PositionManager::updateMarkPrice(const Symbol& symbol, Exchange exchange, double price) {
    if (price <= 0) return;

    std::unique_lock lock(mutex_);

    PositionKey key{std::string(symbol.view()), exchange};
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        Position& pos = it->second;
        pos.markPrice = price;

        if (!pos.isFlat()) {
            double prevUnrealized = pos.unrealizedPnL;
            pos.unrealizedPnL = pos.calculateUnrealizedPnL(price);

            // Update global unrealized PnL
            int64_t deltaCents = static_cast<int64_t>((pos.unrealizedPnL - prevUnrealized) * 100);
            stats_.totalUnrealizedPnLCents.fetch_add(deltaCents, std::memory_order_relaxed);
        }
    }
}

void PositionManager::updateMarkPriceAllExchanges(const Symbol& symbol, double price) {
    if (price <= 0) return;

    std::unique_lock lock(mutex_);
    std::string symbolStr(symbol.view());

    for (auto& [key, pos] : positions_) {
        if (key.symbol == symbolStr) {
            pos.markPrice = price;

            if (!pos.isFlat()) {
                double prevUnrealized = pos.unrealizedPnL;
                pos.unrealizedPnL = pos.calculateUnrealizedPnL(price);

                int64_t deltaCents = static_cast<int64_t>((pos.unrealizedPnL - prevUnrealized) * 100);
                stats_.totalUnrealizedPnLCents.fetch_add(deltaCents, std::memory_order_relaxed);
            }
        }
    }
}

std::optional<Position> PositionManager::getPosition(const Symbol& symbol,
                                                      Exchange exchange) const {
    std::shared_lock lock(mutex_);

    PositionKey key{std::string(symbol.view()), exchange};
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        return it->second;
    }
    return std::nullopt;
}

AggregatedPosition PositionManager::getAggregatedPosition(const Symbol& symbol) const {
    std::shared_lock lock(mutex_);

    AggregatedPosition agg;
    agg.symbol = symbol;
    std::string symbolStr(symbol.view());

    double totalValue = 0.0;
    double totalQty = 0.0;

    for (const auto& [key, pos] : positions_) {
        if (key.symbol == symbolStr) {
            agg.exchangePositions.push_back(pos);
            agg.netQuantity += pos.quantity;
            agg.realizedPnL += pos.realizedPnL;
            agg.unrealizedPnL += pos.unrealizedPnL;
            agg.totalCommissions += pos.totalCommissions;

            // For weighted average entry price
            if (!pos.isFlat()) {
                totalValue += pos.avgEntryPrice * std::abs(pos.quantity);
                totalQty += std::abs(pos.quantity);
            }
        }
    }

    if (totalQty > 0) {
        agg.avgEntryPrice = totalValue / totalQty;
    }

    return agg;
}

std::vector<Position> PositionManager::getPositionsForExchange(Exchange exchange) const {
    std::shared_lock lock(mutex_);

    std::vector<Position> result;
    for (const auto& [key, pos] : positions_) {
        if (key.exchange == exchange) {
            result.push_back(pos);
        }
    }
    return result;
}

std::vector<Position> PositionManager::getPositionsForSymbol(const Symbol& symbol) const {
    std::shared_lock lock(mutex_);

    std::vector<Position> result;
    std::string symbolStr(symbol.view());

    for (const auto& [key, pos] : positions_) {
        if (key.symbol == symbolStr) {
            result.push_back(pos);
        }
    }
    return result;
}

std::vector<Position> PositionManager::getAllOpenPositions() const {
    std::shared_lock lock(mutex_);

    std::vector<Position> result;
    for (const auto& [key, pos] : positions_) {
        if (!pos.isFlat()) {
            result.push_back(pos);
        }
    }
    return result;
}

std::vector<Position> PositionManager::getAllPositions() const {
    std::shared_lock lock(mutex_);

    std::vector<Position> result;
    result.reserve(positions_.size());
    for (const auto& [key, pos] : positions_) {
        result.push_back(pos);
    }
    return result;
}

bool PositionManager::hasPosition(const Symbol& symbol, Exchange exchange) const {
    std::shared_lock lock(mutex_);

    PositionKey key{std::string(symbol.view()), exchange};
    auto it = positions_.find(key);
    return it != positions_.end() && !it->second.isFlat();
}

bool PositionManager::hasAnyPosition(const Symbol& symbol) const {
    std::shared_lock lock(mutex_);
    std::string symbolStr(symbol.view());

    for (const auto& [key, pos] : positions_) {
        if (key.symbol == symbolStr && !pos.isFlat()) {
            return true;
        }
    }
    return false;
}

double PositionManager::getTotalRealizedPnL() const {
    return static_cast<double>(stats_.totalRealizedPnLCents.load(std::memory_order_relaxed)) / 100.0;
}

double PositionManager::getTotalUnrealizedPnL() const {
    std::shared_lock lock(mutex_);

    double total = 0.0;
    for (const auto& [key, pos] : positions_) {
        total += pos.unrealizedPnL;
    }
    return total;
}

double PositionManager::getTotalPnL() const {
    return getTotalRealizedPnL() + getTotalUnrealizedPnL();
}

double PositionManager::getTotalCommissions() const {
    return static_cast<double>(stats_.totalCommissionsCents.load(std::memory_order_relaxed)) / 100.0;
}

double PositionManager::getNetPnL() const {
    return getTotalPnL() - getTotalCommissions();
}

double PositionManager::getSymbolPnL(const Symbol& symbol) const {
    std::shared_lock lock(mutex_);
    std::string symbolStr(symbol.view());

    double total = 0.0;
    for (const auto& [key, pos] : positions_) {
        if (key.symbol == symbolStr) {
            total += pos.totalPnL();
        }
    }
    return total;
}

double PositionManager::getExchangePnL(Exchange exchange) const {
    std::shared_lock lock(mutex_);

    double total = 0.0;
    for (const auto& [key, pos] : positions_) {
        if (key.exchange == exchange) {
            total += pos.totalPnL();
        }
    }
    return total;
}

double PositionManager::getGrossExposure() const {
    std::shared_lock lock(mutex_);

    double total = 0.0;
    for (const auto& [key, pos] : positions_) {
        total += pos.notionalValue();
    }
    return total;
}

double PositionManager::getNetExposure() const {
    std::shared_lock lock(mutex_);

    double total = 0.0;
    for (const auto& [key, pos] : positions_) {
        total += pos.quantity * pos.markPrice;
    }
    return total;
}

size_t PositionManager::getOpenPositionCount() const {
    std::shared_lock lock(mutex_);

    size_t count = 0;
    for (const auto& [key, pos] : positions_) {
        if (!pos.isFlat()) {
            ++count;
        }
    }
    return count;
}

std::optional<Position> PositionManager::getLargestPosition() const {
    std::shared_lock lock(mutex_);

    Position const* largest = nullptr;
    double maxNotional = 0.0;

    for (const auto& [key, pos] : positions_) {
        double notional = pos.notionalValue();
        if (notional > maxNotional) {
            maxNotional = notional;
            largest = &pos;
        }
    }

    if (largest) {
        return *largest;
    }
    return std::nullopt;
}

void PositionManager::setExpectedPosition(const Symbol& symbol, Exchange exchange,
                                          double quantity, double avgPrice) {
    std::unique_lock lock(mutex_);

    PositionKey key{std::string(symbol.view()), exchange};

    Position expected;
    expected.symbol = symbol;
    expected.exchange = exchange;
    expected.quantity = quantity;
    expected.avgEntryPrice = avgPrice;
    expected.lastUpdateTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    expectedPositions_[key] = expected;
}

std::vector<Symbol> PositionManager::checkPositionDiscrepancies() const {
    std::shared_lock lock(mutex_);

    std::vector<Symbol> discrepancies;

    for (const auto& [key, expected] : expectedPositions_) {
        auto it = positions_.find(key);

        double actualQty = 0.0;
        if (it != positions_.end()) {
            actualQty = it->second.quantity;
        }

        // Check for discrepancy (tolerance of 0.0001%)
        double diff = std::abs(actualQty - expected.quantity);
        double tolerance = std::max(std::abs(expected.quantity) * 0.000001, 1e-10);

        if (diff > tolerance) {
            discrepancies.push_back(expected.symbol);
            LOG_WARN("Position discrepancy for {} on {}: expected={}, actual={}",
                     key.symbol, exchange_to_string(key.exchange),
                     expected.quantity, actualQty);
        }
    }

    return discrepancies;
}

void PositionManager::reconcilePosition(const Symbol& symbol, Exchange exchange) {
    std::unique_lock lock(mutex_);

    PositionKey key{std::string(symbol.view()), exchange};

    auto expectedIt = expectedPositions_.find(key);
    if (expectedIt == expectedPositions_.end()) {
        LOG_WARN("No expected position to reconcile for {} on {}",
                 symbol.view(), exchange_to_string(exchange));
        return;
    }

    auto& pos = getOrCreatePosition(symbol, exchange);
    const auto& expected = expectedIt->second;

    double diff = expected.quantity - pos.quantity;

    LOG_INFO("Reconciling position {} on {}: {} -> {} (diff: {})",
             symbol.view(), exchange_to_string(exchange),
             pos.quantity, expected.quantity, diff);

    // Adjust position
    pos.quantity = expected.quantity;
    pos.avgEntryPrice = expected.avgEntryPrice;
    pos.totalCost = expected.quantity * expected.avgEntryPrice;
    pos.lastUpdateTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};
}

void PositionManager::closePosition(const Symbol& symbol, Exchange exchange) {
    std::unique_lock lock(mutex_);

    PositionKey key{std::string(symbol.view()), exchange};
    auto it = positions_.find(key);

    if (it != positions_.end()) {
        Position& pos = it->second;

        if (!pos.isFlat()) {
            // Realize any remaining PnL
            if (pos.markPrice > 0) {
                pos.realizedPnL += pos.calculateUnrealizedPnL(pos.markPrice);
            }

            pos.quantity = 0.0;
            pos.unrealizedPnL = 0.0;
            pos.avgEntryPrice = 0.0;
            pos.totalCost = 0.0;

            stats_.positionsClosed.fetch_add(1, std::memory_order_relaxed);

            LOG_INFO("Position closed: {} on {}", symbol.view(), exchange_to_string(exchange));
        }
    }
}

void PositionManager::flattenAllPositions() {
    std::unique_lock lock(mutex_);

    for (auto& [key, pos] : positions_) {
        if (!pos.isFlat()) {
            if (pos.markPrice > 0) {
                pos.realizedPnL += pos.calculateUnrealizedPnL(pos.markPrice);
            }

            pos.quantity = 0.0;
            pos.unrealizedPnL = 0.0;
            pos.avgEntryPrice = 0.0;
            pos.totalCost = 0.0;

            stats_.positionsClosed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    LOG_INFO("All positions flattened");
}

void PositionManager::reset() {
    std::unique_lock lock(mutex_);

    positions_.clear();
    expectedPositions_.clear();
    resetStats();

    LOG_INFO("PositionManager reset");
}

void PositionManager::setPositionCallback(PositionCallback callback) {
    positionCallback_ = std::move(callback);
}

void PositionManager::setPnLCallback(PnLCallback callback) {
    pnlCallback_ = std::move(callback);
}

void PositionManager::resetStats() {
    stats_.totalFillsProcessed.store(0, std::memory_order_relaxed);
    stats_.positionsOpened.store(0, std::memory_order_relaxed);
    stats_.positionsClosed.store(0, std::memory_order_relaxed);
    stats_.totalRealizedPnLCents.store(0, std::memory_order_relaxed);
    stats_.totalUnrealizedPnLCents.store(0, std::memory_order_relaxed);
    stats_.totalCommissionsCents.store(0, std::memory_order_relaxed);
}

Position& PositionManager::getOrCreatePosition(const Symbol& symbol, Exchange exchange) {
    PositionKey key{std::string(symbol.view()), exchange};

    auto it = positions_.find(key);
    if (it != positions_.end()) {
        return it->second;
    }

    Position pos;
    pos.symbol = symbol;
    pos.exchange = exchange;
    pos.openTime = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    auto [insertIt, inserted] = positions_.emplace(key, pos);
    return insertIt->second;
}

void PositionManager::notifyPositionUpdate(const PositionUpdate& update) {
    if (positionCallback_) {
        positionCallback_(update);
    }
}

void PositionManager::notifyPnLUpdate(const Symbol& symbol, double realizedPnL, double unrealizedPnL) {
    if (pnlCallback_) {
        pnlCallback_(symbol, realizedPnL, unrealizedPnL);
    }
}

double PositionManager::processPositionChange(Position& pos, Side side, double price, double quantity) {
    double realizedPnL = 0.0;
    double signedQty = (side == Side::Buy) ? quantity : -quantity;

    // Check if this reduces or increases position
    bool isIncrease = (pos.isFlat()) ||
                      (pos.isLong() && side == Side::Buy) ||
                      (pos.isShort() && side == Side::Sell);

    if (isIncrease) {
        // Increasing position - update average entry price
        double newTotalCost = pos.totalCost + (price * signedQty);
        double newQuantity = pos.quantity + signedQty;

        if (std::abs(newQuantity) > 1e-10) {
            pos.avgEntryPrice = std::abs(newTotalCost / newQuantity);
        }

        pos.quantity = newQuantity;
        pos.totalCost = newTotalCost;
    } else {
        // Reducing or reversing position
        double closeQty = std::min(std::abs(quantity), std::abs(pos.quantity));
        double remainingQty = quantity - closeQty;

        // Calculate realized PnL for closed portion
        if (pos.isLong()) {
            // Closing long: sell price - entry price
            realizedPnL = closeQty * (price - pos.avgEntryPrice);
        } else {
            // Closing short: entry price - buy price
            realizedPnL = closeQty * (pos.avgEntryPrice - price);
        }

        pos.realizedPnL += realizedPnL;

        // Update position
        if (side == Side::Buy) {
            pos.quantity += closeQty; // Reduce short
        } else {
            pos.quantity -= closeQty; // Reduce long
        }

        // Handle position reversal
        if (remainingQty > 1e-10) {
            // Reversed position - new entry at current price
            pos.quantity = (side == Side::Buy) ? remainingQty : -remainingQty;
            pos.avgEntryPrice = price;
            pos.totalCost = pos.quantity * price;
        } else if (pos.isFlat()) {
            // Position fully closed
            pos.avgEntryPrice = 0.0;
            pos.totalCost = 0.0;
        } else {
            // Position reduced but not closed - maintain average entry
            pos.totalCost = pos.quantity * pos.avgEntryPrice;
        }
    }

    return realizedPnL;
}

} // namespace hft::oms
