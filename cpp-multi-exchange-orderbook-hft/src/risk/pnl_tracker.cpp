/**
 * @file pnl_tracker.cpp
 * @brief Implementation of PnL Tracker
 */

#include "risk/pnl_tracker.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace hft::risk {

// ============================================================================
// PnLTracker Implementation
// ============================================================================

PnLTracker::PnLTracker() {
    currentDayStart_ = std::chrono::system_clock::now();
    LOG_INFO("PnLTracker initialized");
}

PnLTracker::PnLTracker(std::shared_ptr<PositionManager> positionManager)
    : PnLTracker()
{
    positionManager_ = std::move(positionManager);
}

PnLTracker::~PnLTracker() {
    double finalPnL = getTotalPnL();
    LOG_INFO("PnLTracker shutting down - final PnL: {}", finalPnL);
}

void PnLTracker::recordPnL(const PnLEvent& event) {
    // Update atomic counters
    int64_t pnlCents = doubleToCents(event.pnl);
    int64_t commCents = doubleToCents(event.commission);

    realizedPnLCents_.fetch_add(pnlCents, std::memory_order_relaxed);
    commissionsCents_.fetch_add(commCents, std::memory_order_relaxed);
    tradeCount_.fetch_add(1, std::memory_order_relaxed);

    if (event.isWinningTrade()) {
        winningTrades_.fetch_add(1, std::memory_order_relaxed);
        grossProfitCents_.fetch_add(pnlCents, std::memory_order_relaxed);
    } else if (event.netPnl < 0) {
        losingTrades_.fetch_add(1, std::memory_order_relaxed);
        grossLossCents_.fetch_add(std::abs(pnlCents), std::memory_order_relaxed);
    }

    // Store in history
    {
        std::unique_lock lock(mutex_);
        recentTrades_.push_back(event);
        if (recentTrades_.size() > MAX_TRADE_HISTORY) {
            recentTrades_.pop_front();
        }
    }

    // Update peak and drawdown
    updatePeakAndDrawdown();

    // Check limits
    checkLimits();

    // Notify
    notifyUpdate();

    // Check for large trades
    double absNetPnl = std::abs(event.netPnl);
    if (absNetPnl > 10000) { // Configurable threshold
        if (event.netPnl > 0) {
            notifyAlert(PnLAlertType::LargeWin, event.netPnl,
                        "Large winning trade: " + std::to_string(event.netPnl));
        } else {
            notifyAlert(PnLAlertType::LargeLoss, event.netPnl,
                        "Large losing trade: " + std::to_string(event.netPnl));
        }
    }

    LOG_DEBUG("PnL recorded: {} (net: {})", event.pnl, event.netPnl);
}

void PnLTracker::recordRealizedPnL(double pnl, const Symbol& symbol, Exchange exchange) {
    int64_t pnlCents = doubleToCents(pnl);
    realizedPnLCents_.fetch_add(pnlCents, std::memory_order_relaxed);
    tradeCount_.fetch_add(1, std::memory_order_relaxed);

    if (pnl > 0) {
        winningTrades_.fetch_add(1, std::memory_order_relaxed);
        grossProfitCents_.fetch_add(pnlCents, std::memory_order_relaxed);
    } else if (pnl < 0) {
        losingTrades_.fetch_add(1, std::memory_order_relaxed);
        grossLossCents_.fetch_add(std::abs(pnlCents), std::memory_order_relaxed);
    }

    updatePeakAndDrawdown();
    checkLimits();
    notifyUpdate();
}

void PnLTracker::recordCommission(double commission) {
    int64_t commCents = doubleToCents(commission);
    commissionsCents_.fetch_add(commCents, std::memory_order_relaxed);
}

void PnLTracker::updateUnrealizedPnL(double unrealizedPnL) {
    unrealizedPnLCents_.store(doubleToCents(unrealizedPnL), std::memory_order_relaxed);
    updatePeakAndDrawdown();
    checkLimits();
}

PnLSnapshot PnLTracker::getSnapshot() const {
    PnLSnapshot snapshot;

    snapshot.timestamp = Timestamp{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count())};

    snapshot.realizedPnL = centsToDouble(realizedPnLCents_.load(std::memory_order_relaxed));
    snapshot.unrealizedPnL = centsToDouble(unrealizedPnLCents_.load(std::memory_order_relaxed));
    snapshot.totalPnL = snapshot.realizedPnL + snapshot.unrealizedPnL;

    snapshot.commissions = centsToDouble(commissionsCents_.load(std::memory_order_relaxed));
    snapshot.netPnL = snapshot.totalPnL - snapshot.commissions;

    snapshot.peakPnL = centsToDouble(peakPnLCents_.load(std::memory_order_relaxed));

    double currentPnL = snapshot.netPnL;
    snapshot.drawdown = snapshot.peakPnL > currentPnL ? snapshot.peakPnL - currentPnL : 0.0;
    snapshot.maxDrawdown = centsToDouble(maxDrawdownCents_.load(std::memory_order_relaxed));

    // Daily PnL
    double dailyStart = centsToDouble(dailyStartPnLCents_.load(std::memory_order_relaxed));
    snapshot.dailyPnL = snapshot.netPnL - dailyStart;

    // Trade statistics
    snapshot.tradeCount = tradeCount_.load(std::memory_order_relaxed);
    uint64_t wins = winningTrades_.load(std::memory_order_relaxed);
    snapshot.winRate = snapshot.tradeCount > 0 ?
        (static_cast<double>(wins) / snapshot.tradeCount) * 100.0 : 0.0;

    snapshot.avgPnLPerTrade = snapshot.tradeCount > 0 ?
        snapshot.realizedPnL / snapshot.tradeCount : 0.0;

    return snapshot;
}

double PnLTracker::getTotalPnL() const {
    return centsToDouble(realizedPnLCents_.load(std::memory_order_relaxed) +
                         unrealizedPnLCents_.load(std::memory_order_relaxed));
}

double PnLTracker::getRealizedPnL() const {
    return centsToDouble(realizedPnLCents_.load(std::memory_order_relaxed));
}

double PnLTracker::getUnrealizedPnL() const {
    return centsToDouble(unrealizedPnLCents_.load(std::memory_order_relaxed));
}

double PnLTracker::getNetPnL() const {
    return getTotalPnL() - centsToDouble(commissionsCents_.load(std::memory_order_relaxed));
}

double PnLTracker::getDailyPnL() const {
    double currentPnL = getNetPnL();
    double dailyStart = centsToDouble(dailyStartPnLCents_.load(std::memory_order_relaxed));
    return currentPnL - dailyStart;
}

double PnLTracker::getDrawdown() const {
    double peakPnL = centsToDouble(peakPnLCents_.load(std::memory_order_relaxed));
    double currentPnL = getNetPnL();
    return peakPnL > currentPnL ? peakPnL - currentPnL : 0.0;
}

double PnLTracker::getMaxDrawdown() const {
    return centsToDouble(maxDrawdownCents_.load(std::memory_order_relaxed));
}

double PnLTracker::getPeakPnL() const {
    return centsToDouble(peakPnLCents_.load(std::memory_order_relaxed));
}

std::vector<DailyPnL> PnLTracker::getDailyHistory(size_t days) const {
    std::shared_lock lock(mutex_);

    size_t count = std::min(days, dailyHistory_.size());
    std::vector<DailyPnL> result;
    result.reserve(count);

    auto it = dailyHistory_.rbegin();
    for (size_t i = 0; i < count && it != dailyHistory_.rend(); ++i, ++it) {
        result.push_back(*it);
    }

    return result;
}

DailyPnL PnLTracker::getTodayPnL() const {
    DailyPnL today;
    today.date = currentDayStart_;
    today.openingPnL = centsToDouble(dailyStartPnLCents_.load(std::memory_order_relaxed));
    today.closingPnL = getNetPnL();
    today.dailyChange = today.closingPnL - today.openingPnL;
    today.realizedPnL = getRealizedPnL();
    today.unrealizedPnL = getUnrealizedPnL();
    today.commissions = centsToDouble(commissionsCents_.load(std::memory_order_relaxed));
    today.tradeCount = tradeCount_.load(std::memory_order_relaxed);
    today.winningTrades = winningTrades_.load(std::memory_order_relaxed);
    today.losingTrades = losingTrades_.load(std::memory_order_relaxed);
    today.grossProfit = centsToDouble(grossProfitCents_.load(std::memory_order_relaxed));
    today.grossLoss = centsToDouble(grossLossCents_.load(std::memory_order_relaxed));

    return today;
}

std::vector<PnLSnapshot> PnLTracker::getHistory(size_t count) const {
    std::shared_lock lock(mutex_);

    size_t histCount = std::min(count, snapshotHistory_.size());
    std::vector<PnLSnapshot> result;
    result.reserve(histCount);

    auto it = snapshotHistory_.rbegin();
    for (size_t i = 0; i < histCount && it != snapshotHistory_.rend(); ++i, ++it) {
        result.push_back(*it);
    }

    return result;
}

std::vector<PnLEvent> PnLTracker::getRecentTrades(size_t count) const {
    std::shared_lock lock(mutex_);

    size_t tradeCount = std::min(count, recentTrades_.size());
    std::vector<PnLEvent> result;
    result.reserve(tradeCount);

    auto it = recentTrades_.rbegin();
    for (size_t i = 0; i < tradeCount && it != recentTrades_.rend(); ++i, ++it) {
        result.push_back(*it);
    }

    return result;
}

uint64_t PnLTracker::getTradeCount() const {
    return tradeCount_.load(std::memory_order_relaxed);
}

uint64_t PnLTracker::getWinningTradeCount() const {
    return winningTrades_.load(std::memory_order_relaxed);
}

uint64_t PnLTracker::getLosingTradeCount() const {
    return losingTrades_.load(std::memory_order_relaxed);
}

double PnLTracker::getWinRate() const {
    uint64_t total = tradeCount_.load(std::memory_order_relaxed);
    if (total == 0) return 0.0;

    uint64_t wins = winningTrades_.load(std::memory_order_relaxed);
    return (static_cast<double>(wins) / total) * 100.0;
}

double PnLTracker::getAvgPnLPerTrade() const {
    uint64_t total = tradeCount_.load(std::memory_order_relaxed);
    if (total == 0) return 0.0;

    return getRealizedPnL() / total;
}

double PnLTracker::getProfitFactor() const {
    double grossLoss = centsToDouble(grossLossCents_.load(std::memory_order_relaxed));
    if (grossLoss <= 0) return 0.0;

    double grossProfit = centsToDouble(grossProfitCents_.load(std::memory_order_relaxed));
    return grossProfit / grossLoss;
}

double PnLTracker::getSharpeRatio() const {
    std::shared_lock lock(mutex_);

    if (dailyHistory_.size() < 30) {
        return 0.0; // Not enough data
    }

    // Calculate daily returns
    std::vector<double> dailyReturns;
    dailyReturns.reserve(dailyHistory_.size());

    for (const auto& day : dailyHistory_) {
        dailyReturns.push_back(day.dailyChange);
    }

    // Calculate mean return
    double meanReturn = std::accumulate(dailyReturns.begin(), dailyReturns.end(), 0.0) /
                        dailyReturns.size();

    // Calculate standard deviation
    double variance = 0.0;
    for (double ret : dailyReturns) {
        variance += (ret - meanReturn) * (ret - meanReturn);
    }
    variance /= dailyReturns.size();
    double stdDev = std::sqrt(variance);

    if (stdDev <= 0) return 0.0;

    // Annualized Sharpe (assuming 252 trading days)
    return (meanReturn / stdDev) * std::sqrt(252.0);
}

void PnLTracker::setLimits(const PnLLimits& limits) {
    std::unique_lock lock(mutex_);
    limits_ = limits;
    LOG_INFO("PnL limits updated: maxDailyLoss={}, maxDrawdown={}",
             limits.maxDailyLoss, limits.maxDrawdown);
}

const PnLLimits& PnLTracker::getLimits() const {
    std::shared_lock lock(mutex_);
    return limits_;
}

bool PnLTracker::isDailyLossLimitReached() const {
    return dailyLossLimitReached_.load(std::memory_order_relaxed);
}

bool PnLTracker::isDrawdownLimitReached() const {
    return drawdownLimitReached_.load(std::memory_order_relaxed);
}

bool PnLTracker::isProfitTargetReached() const {
    return profitTargetReached_.load(std::memory_order_relaxed);
}

bool PnLTracker::shouldStopTrading() const {
    std::shared_lock lock(mutex_);

    if (limits_.stopOnDailyLoss && isDailyLossLimitReached()) {
        return true;
    }

    if (limits_.stopOnDrawdown && isDrawdownLimitReached()) {
        return true;
    }

    if (limits_.stopOnProfitTarget && isProfitTargetReached()) {
        return true;
    }

    return false;
}

double PnLTracker::getRemainingDailyLossCapacity() const {
    std::shared_lock lock(mutex_);

    double dailyPnL = getDailyPnL();
    if (dailyPnL >= 0) {
        return limits_.maxDailyLoss;
    }

    return std::max(0.0, limits_.maxDailyLoss - std::abs(dailyPnL));
}

void PnLTracker::startNewDay() {
    // Save current day's summary
    DailyPnL today = getTodayPnL();

    {
        std::unique_lock lock(mutex_);
        dailyHistory_.push_back(today);
        if (dailyHistory_.size() > MAX_DAILY_HISTORY) {
            dailyHistory_.pop_front();
        }
    }

    // Reset daily tracking
    dailyStartPnLCents_.store(doubleToCents(getNetPnL()), std::memory_order_relaxed);
    currentDayStart_ = std::chrono::system_clock::now();

    // Reset daily flags
    dailyLossLimitReached_.store(false, std::memory_order_relaxed);
    profitTargetReached_.store(false, std::memory_order_relaxed);

    LOG_INFO("Started new trading day - previous day PnL: {}", today.dailyChange);
}

void PnLTracker::endDay() {
    // Take final snapshot
    takeSnapshot();

    // Save daily summary
    DailyPnL today = getTodayPnL();

    {
        std::unique_lock lock(mutex_);
        dailyHistory_.push_back(today);
        if (dailyHistory_.size() > MAX_DAILY_HISTORY) {
            dailyHistory_.pop_front();
        }
    }

    LOG_INFO("Ended trading day - PnL: {}", today.dailyChange);
}

void PnLTracker::reset() {
    realizedPnLCents_.store(0, std::memory_order_relaxed);
    unrealizedPnLCents_.store(0, std::memory_order_relaxed);
    commissionsCents_.store(0, std::memory_order_relaxed);
    peakPnLCents_.store(0, std::memory_order_relaxed);
    maxDrawdownCents_.store(0, std::memory_order_relaxed);
    dailyStartPnLCents_.store(0, std::memory_order_relaxed);

    tradeCount_.store(0, std::memory_order_relaxed);
    winningTrades_.store(0, std::memory_order_relaxed);
    losingTrades_.store(0, std::memory_order_relaxed);
    grossProfitCents_.store(0, std::memory_order_relaxed);
    grossLossCents_.store(0, std::memory_order_relaxed);

    dailyLossLimitReached_.store(false, std::memory_order_relaxed);
    drawdownLimitReached_.store(false, std::memory_order_relaxed);
    profitTargetReached_.store(false, std::memory_order_relaxed);

    {
        std::unique_lock lock(mutex_);
        snapshotHistory_.clear();
        dailyHistory_.clear();
        recentTrades_.clear();
    }

    currentDayStart_ = std::chrono::system_clock::now();

    LOG_INFO("PnL tracker reset");
}

void PnLTracker::setAlertCallback(PnLAlertCallback callback) {
    alertCallback_ = std::move(callback);
}

void PnLTracker::setUpdateCallback(PnLUpdateCallback callback) {
    updateCallback_ = std::move(callback);
}

void PnLTracker::setPositionManager(std::shared_ptr<PositionManager> positionManager) {
    positionManager_ = std::move(positionManager);
}

void PnLTracker::syncFromPositionManager() {
    if (!positionManager_) return;

    double unrealized = positionManager_->getTotalUnrealizedPnL();
    double realized = positionManager_->getTotalRealizedPnL();
    double commissions = positionManager_->getTotalCommissions();

    unrealizedPnLCents_.store(doubleToCents(unrealized), std::memory_order_relaxed);

    // Optionally sync realized (depends on whether PM tracks cumulative)
    // realizedPnLCents_.store(doubleToCents(realized), std::memory_order_relaxed);

    updatePeakAndDrawdown();
    checkLimits();
}

void PnLTracker::updatePeakAndDrawdown() {
    double currentPnL = getNetPnL();
    int64_t currentCents = doubleToCents(currentPnL);

    // Update peak (high water mark)
    int64_t currentPeak = peakPnLCents_.load(std::memory_order_relaxed);
    while (currentCents > currentPeak) {
        if (peakPnLCents_.compare_exchange_weak(currentPeak, currentCents,
            std::memory_order_relaxed)) {
            notifyAlert(PnLAlertType::NewHighWaterMark, currentPnL,
                        "New peak PnL: " + std::to_string(currentPnL));
            break;
        }
    }

    // Update max drawdown
    double peakPnL = centsToDouble(peakPnLCents_.load(std::memory_order_relaxed));
    double drawdown = peakPnL > currentPnL ? peakPnL - currentPnL : 0.0;

    int64_t drawdownCents = doubleToCents(drawdown);
    int64_t currentMaxDD = maxDrawdownCents_.load(std::memory_order_relaxed);
    while (drawdownCents > currentMaxDD) {
        if (maxDrawdownCents_.compare_exchange_weak(currentMaxDD, drawdownCents,
            std::memory_order_relaxed)) {
            break;
        }
    }
}

void PnLTracker::checkLimits() {
    std::shared_lock lock(mutex_);

    double dailyPnL = getDailyPnL();
    double drawdown = getDrawdown();
    double peakPnL = getPeakPnL();

    // Check daily loss limit
    if (limits_.stopOnDailyLoss && dailyPnL < -limits_.maxDailyLoss) {
        bool expected = false;
        if (dailyLossLimitReached_.compare_exchange_strong(expected, true,
            std::memory_order_relaxed)) {
            notifyAlert(PnLAlertType::DailyLossReached, dailyPnL,
                        "Daily loss limit reached: " + std::to_string(dailyPnL));
        }
    } else if (dailyPnL < -limits_.maxDailyLoss * 0.8) {
        notifyAlert(PnLAlertType::DailyLossWarning, dailyPnL,
                    "Approaching daily loss limit: " + std::to_string(dailyPnL));
    }

    // Check drawdown limit
    if (limits_.stopOnDrawdown && drawdown > limits_.maxDrawdown) {
        bool expected = false;
        if (drawdownLimitReached_.compare_exchange_strong(expected, true,
            std::memory_order_relaxed)) {
            notifyAlert(PnLAlertType::DrawdownReached, drawdown,
                        "Max drawdown reached: " + std::to_string(drawdown));
        }
    } else if (drawdown > limits_.maxDrawdown * 0.8) {
        notifyAlert(PnLAlertType::DrawdownWarning, drawdown,
                    "Approaching max drawdown: " + std::to_string(drawdown));
    }

    // Check profit target
    if (limits_.stopOnProfitTarget && dailyPnL > limits_.dailyProfitTarget) {
        bool expected = false;
        if (profitTargetReached_.compare_exchange_strong(expected, true,
            std::memory_order_relaxed)) {
            notifyAlert(PnLAlertType::ProfitTargetReached, dailyPnL,
                        "Daily profit target reached: " + std::to_string(dailyPnL));
        }
    }
}

void PnLTracker::notifyAlert(PnLAlertType type, double pnl, const std::string& message) {
    LOG_WARN("PnL Alert: {} - {}", pnl_alert_to_string(type), message);

    if (alertCallback_) {
        alertCallback_(type, pnl, message);
    }
}

void PnLTracker::notifyUpdate() {
    if (updateCallback_) {
        updateCallback_(getSnapshot());
    }
}

void PnLTracker::takeSnapshot() {
    PnLSnapshot snapshot = getSnapshot();

    std::unique_lock lock(mutex_);
    snapshotHistory_.push_back(snapshot);
    if (snapshotHistory_.size() > MAX_SNAPSHOT_HISTORY) {
        snapshotHistory_.pop_front();
    }
}

// ============================================================================
// PnLTrackerBuilder Implementation
// ============================================================================

PnLTrackerBuilder& PnLTrackerBuilder::withPositionManager(std::shared_ptr<PositionManager> pm) {
    positionManager_ = std::move(pm);
    return *this;
}

PnLTrackerBuilder& PnLTrackerBuilder::withLimits(const PnLLimits& limits) {
    limits_ = limits;
    return *this;
}

PnLTrackerBuilder& PnLTrackerBuilder::withAlertCallback(PnLAlertCallback callback) {
    alertCallback_ = std::move(callback);
    return *this;
}

PnLTrackerBuilder& PnLTrackerBuilder::withUpdateCallback(PnLUpdateCallback callback) {
    updateCallback_ = std::move(callback);
    return *this;
}

std::unique_ptr<PnLTracker> PnLTrackerBuilder::build() {
    auto tracker = std::make_unique<PnLTracker>(positionManager_);

    tracker->setLimits(limits_);

    if (alertCallback_) {
        tracker->setAlertCallback(std::move(alertCallback_));
    }

    if (updateCallback_) {
        tracker->setUpdateCallback(std::move(updateCallback_));
    }

    return tracker;
}

} // namespace hft::risk
