#pragma once

/**
 * @file pnl_tracker.hpp
 * @brief Real-time PnL Tracking for HFT trading system
 *
 * Provides comprehensive PnL management including:
 * - Real-time PnL calculation
 * - Daily PnL limits (stop trading on max loss)
 * - PnL history tracking
 * - Mark-to-market calculations
 */

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <atomic>
#include <optional>
#include <chrono>

#include "core/types.hpp"
#include "oms/position_manager.hpp"

namespace hft::risk {

using namespace hft::core;
using namespace hft::oms;

/**
 * @brief PnL snapshot at a point in time
 */
struct PnLSnapshot {
    Timestamp timestamp;

    double realizedPnL{0.0};
    double unrealizedPnL{0.0};
    double totalPnL{0.0};

    double commissions{0.0};
    double netPnL{0.0};

    double dailyPnL{0.0};
    double weeklyPnL{0.0};
    double monthlyPnL{0.0};

    double peakPnL{0.0};          // High water mark
    double drawdown{0.0};          // Current drawdown from peak
    double maxDrawdown{0.0};       // Maximum drawdown observed

    uint64_t tradeCount{0};
    double avgPnLPerTrade{0.0};
    double winRate{0.0};           // Percentage of winning trades

    [[nodiscard]] double getDrawdownPct() const noexcept {
        return peakPnL > 0 ? (drawdown / peakPnL) * 100.0 : 0.0;
    }
};

/**
 * @brief Daily PnL summary
 */
struct DailyPnL {
    std::chrono::system_clock::time_point date;

    double openingPnL{0.0};
    double closingPnL{0.0};
    double dailyChange{0.0};

    double highPnL{0.0};
    double lowPnL{0.0};

    double realizedPnL{0.0};
    double unrealizedPnL{0.0};
    double commissions{0.0};

    uint64_t tradeCount{0};
    uint64_t winningTrades{0};
    uint64_t losingTrades{0};

    double grossProfit{0.0};
    double grossLoss{0.0};

    [[nodiscard]] double netPnL() const noexcept {
        return realizedPnL + unrealizedPnL - commissions;
    }

    [[nodiscard]] double profitFactor() const noexcept {
        return grossLoss > 0 ? grossProfit / std::abs(grossLoss) : 0.0;
    }

    [[nodiscard]] double winRate() const noexcept {
        return tradeCount > 0 ?
            (static_cast<double>(winningTrades) / tradeCount) * 100.0 : 0.0;
    }
};

/**
 * @brief PnL event for tracking individual trade PnL
 */
struct PnLEvent {
    uint64_t tradeId{0};
    Symbol symbol;
    Exchange exchange{Exchange::Unknown};

    Side side{Side::Buy};
    double quantity{0.0};
    double entryPrice{0.0};
    double exitPrice{0.0};

    double pnl{0.0};
    double commission{0.0};
    double netPnl{0.0};

    Timestamp entryTime;
    Timestamp exitTime;

    [[nodiscard]] double holdingTimeMs() const noexcept {
        return static_cast<double>(exitTime.nanos - entryTime.nanos) / 1e6;
    }

    [[nodiscard]] bool isWinningTrade() const noexcept {
        return netPnl > 0;
    }
};

/**
 * @brief PnL limits configuration
 */
struct PnLLimits {
    double maxDailyLoss{100000.0};      // Stop trading after this loss
    double maxDrawdown{50000.0};         // Max drawdown from peak
    double maxDrawdownPct{10.0};         // Max drawdown percentage
    double dailyProfitTarget{200000.0}; // Optional: stop after target hit
    double maxPositionLoss{10000.0};     // Max loss per position

    bool stopOnDailyLoss{true};
    bool stopOnDrawdown{true};
    bool stopOnProfitTarget{false};
};

/**
 * @brief PnL alert types
 */
enum class PnLAlertType : uint8_t {
    DailyLossWarning = 0,    // Approaching daily loss limit
    DailyLossReached = 1,    // Daily loss limit hit
    DrawdownWarning = 2,     // Approaching max drawdown
    DrawdownReached = 3,     // Max drawdown hit
    ProfitTargetReached = 4, // Daily profit target hit
    NewHighWaterMark = 5,    // New peak PnL achieved
    LargeWin = 6,            // Unusually large win
    LargeLoss = 7            // Unusually large loss
};

[[nodiscard]] constexpr std::string_view pnl_alert_to_string(PnLAlertType alert) noexcept {
    switch (alert) {
        case PnLAlertType::DailyLossWarning: return "DAILY_LOSS_WARNING";
        case PnLAlertType::DailyLossReached: return "DAILY_LOSS_REACHED";
        case PnLAlertType::DrawdownWarning: return "DRAWDOWN_WARNING";
        case PnLAlertType::DrawdownReached: return "DRAWDOWN_REACHED";
        case PnLAlertType::ProfitTargetReached: return "PROFIT_TARGET_REACHED";
        case PnLAlertType::NewHighWaterMark: return "NEW_HIGH_WATER_MARK";
        case PnLAlertType::LargeWin: return "LARGE_WIN";
        case PnLAlertType::LargeLoss: return "LARGE_LOSS";
    }
    return "UNKNOWN";
}

/**
 * @brief PnL alert callback
 */
using PnLAlertCallback = std::function<void(PnLAlertType, double pnl, const std::string& message)>;
using PnLUpdateCallback = std::function<void(const PnLSnapshot&)>;

/**
 * @brief PnL Tracker
 *
 * Thread-safe PnL tracking with support for:
 * - Real-time PnL calculation
 * - Daily limits
 * - Historical tracking
 * - Drawdown monitoring
 */
class PnLTracker {
public:
    /**
     * @brief Constructor
     */
    PnLTracker();

    /**
     * @brief Constructor with position manager
     */
    explicit PnLTracker(std::shared_ptr<PositionManager> positionManager);

    /**
     * @brief Destructor
     */
    ~PnLTracker();

    // Non-copyable
    PnLTracker(const PnLTracker&) = delete;
    PnLTracker& operator=(const PnLTracker&) = delete;

    // ===== PnL Recording =====

    /**
     * @brief Record a realized PnL event
     */
    void recordPnL(const PnLEvent& event);

    /**
     * @brief Record realized PnL
     */
    void recordRealizedPnL(double pnl, const Symbol& symbol = Symbol{},
                           Exchange exchange = Exchange::Unknown);

    /**
     * @brief Record commission
     */
    void recordCommission(double commission);

    /**
     * @brief Update unrealized PnL (call periodically with position MTM)
     */
    void updateUnrealizedPnL(double unrealizedPnL);

    // ===== PnL Queries =====

    /**
     * @brief Get current PnL snapshot
     */
    [[nodiscard]] PnLSnapshot getSnapshot() const;

    /**
     * @brief Get current total PnL
     */
    [[nodiscard]] double getTotalPnL() const;

    /**
     * @brief Get realized PnL
     */
    [[nodiscard]] double getRealizedPnL() const;

    /**
     * @brief Get unrealized PnL
     */
    [[nodiscard]] double getUnrealizedPnL() const;

    /**
     * @brief Get net PnL (after commissions)
     */
    [[nodiscard]] double getNetPnL() const;

    /**
     * @brief Get today's PnL
     */
    [[nodiscard]] double getDailyPnL() const;

    /**
     * @brief Get current drawdown
     */
    [[nodiscard]] double getDrawdown() const;

    /**
     * @brief Get maximum drawdown observed
     */
    [[nodiscard]] double getMaxDrawdown() const;

    /**
     * @brief Get peak PnL (high water mark)
     */
    [[nodiscard]] double getPeakPnL() const;

    // ===== PnL History =====

    /**
     * @brief Get daily PnL history
     */
    [[nodiscard]] std::vector<DailyPnL> getDailyHistory(size_t days = 30) const;

    /**
     * @brief Get today's daily PnL
     */
    [[nodiscard]] DailyPnL getTodayPnL() const;

    /**
     * @brief Get PnL history (snapshots)
     */
    [[nodiscard]] std::vector<PnLSnapshot> getHistory(size_t count = 100) const;

    /**
     * @brief Get recent trades
     */
    [[nodiscard]] std::vector<PnLEvent> getRecentTrades(size_t count = 100) const;

    // ===== Statistics =====

    /**
     * @brief Get total trade count
     */
    [[nodiscard]] uint64_t getTradeCount() const;

    /**
     * @brief Get winning trade count
     */
    [[nodiscard]] uint64_t getWinningTradeCount() const;

    /**
     * @brief Get losing trade count
     */
    [[nodiscard]] uint64_t getLosingTradeCount() const;

    /**
     * @brief Get win rate percentage
     */
    [[nodiscard]] double getWinRate() const;

    /**
     * @brief Get average PnL per trade
     */
    [[nodiscard]] double getAvgPnLPerTrade() const;

    /**
     * @brief Get profit factor (gross profit / gross loss)
     */
    [[nodiscard]] double getProfitFactor() const;

    /**
     * @brief Get Sharpe ratio estimate (requires sufficient history)
     */
    [[nodiscard]] double getSharpeRatio() const;

    // ===== Limit Management =====

    /**
     * @brief Set PnL limits
     */
    void setLimits(const PnLLimits& limits);

    /**
     * @brief Get current limits
     */
    [[nodiscard]] const PnLLimits& getLimits() const;

    /**
     * @brief Check if daily loss limit is reached
     */
    [[nodiscard]] bool isDailyLossLimitReached() const;

    /**
     * @brief Check if max drawdown is reached
     */
    [[nodiscard]] bool isDrawdownLimitReached() const;

    /**
     * @brief Check if profit target is reached
     */
    [[nodiscard]] bool isProfitTargetReached() const;

    /**
     * @brief Check if trading should be stopped based on PnL
     */
    [[nodiscard]] bool shouldStopTrading() const;

    /**
     * @brief Get remaining loss capacity for today
     */
    [[nodiscard]] double getRemainingDailyLossCapacity() const;

    // ===== Day Management =====

    /**
     * @brief Start new trading day
     */
    void startNewDay();

    /**
     * @brief End trading day
     */
    void endDay();

    /**
     * @brief Reset all tracking (for testing)
     */
    void reset();

    // ===== Callbacks =====

    /**
     * @brief Set PnL alert callback
     */
    void setAlertCallback(PnLAlertCallback callback);

    /**
     * @brief Set PnL update callback
     */
    void setUpdateCallback(PnLUpdateCallback callback);

    // ===== Position Manager Integration =====

    /**
     * @brief Set position manager for unrealized PnL updates
     */
    void setPositionManager(std::shared_ptr<PositionManager> positionManager);

    /**
     * @brief Update from position manager (call periodically)
     */
    void syncFromPositionManager();

private:
    std::shared_ptr<PositionManager> positionManager_;

    mutable std::shared_mutex mutex_;

    // Current state
    std::atomic<int64_t> realizedPnLCents_{0};
    std::atomic<int64_t> unrealizedPnLCents_{0};
    std::atomic<int64_t> commissionsCents_{0};
    std::atomic<int64_t> peakPnLCents_{0};
    std::atomic<int64_t> maxDrawdownCents_{0};

    // Day tracking
    std::atomic<int64_t> dailyStartPnLCents_{0};
    std::chrono::system_clock::time_point currentDayStart_;

    // Trade statistics (atomic for thread safety)
    std::atomic<uint64_t> tradeCount_{0};
    std::atomic<uint64_t> winningTrades_{0};
    std::atomic<uint64_t> losingTrades_{0};
    std::atomic<int64_t> grossProfitCents_{0};
    std::atomic<int64_t> grossLossCents_{0};

    // History (protected by mutex_)
    std::deque<PnLSnapshot> snapshotHistory_;
    std::deque<DailyPnL> dailyHistory_;
    std::deque<PnLEvent> recentTrades_;

    static constexpr size_t MAX_SNAPSHOT_HISTORY = 10000;
    static constexpr size_t MAX_DAILY_HISTORY = 365;
    static constexpr size_t MAX_TRADE_HISTORY = 10000;

    // Limits
    PnLLimits limits_;

    // Callbacks
    PnLAlertCallback alertCallback_;
    PnLUpdateCallback updateCallback_;

    // Internal state tracking
    std::atomic<bool> dailyLossLimitReached_{false};
    std::atomic<bool> drawdownLimitReached_{false};
    std::atomic<bool> profitTargetReached_{false};

    // Helper methods
    void updatePeakAndDrawdown();
    void checkLimits();
    void notifyAlert(PnLAlertType type, double pnl, const std::string& message);
    void notifyUpdate();
    void takeSnapshot();
    double centsToDouble(int64_t cents) const { return static_cast<double>(cents) / 100.0; }
    int64_t doubleToCents(double val) const { return static_cast<int64_t>(val * 100.0); }
};

/**
 * @brief PnL tracker builder
 */
class PnLTrackerBuilder {
public:
    PnLTrackerBuilder& withPositionManager(std::shared_ptr<PositionManager> pm);
    PnLTrackerBuilder& withLimits(const PnLLimits& limits);
    PnLTrackerBuilder& withAlertCallback(PnLAlertCallback callback);
    PnLTrackerBuilder& withUpdateCallback(PnLUpdateCallback callback);

    std::unique_ptr<PnLTracker> build();

private:
    std::shared_ptr<PositionManager> positionManager_;
    PnLLimits limits_;
    PnLAlertCallback alertCallback_;
    PnLUpdateCallback updateCallback_;
};

} // namespace hft::risk
