#pragma once

/**
 * @file position_limits.hpp
 * @brief Position Limits Management for HFT trading system
 *
 * Provides configurable position limits including:
 * - Per-symbol position limits
 * - Per-exchange limits
 * - Total portfolio limits
 * - YAML configuration support
 */

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <filesystem>

#include "core/types.hpp"
#include "oms/position_manager.hpp"

namespace hft::risk {

using namespace hft::core;
using namespace hft::oms;

/**
 * @brief Position limit type
 */
enum class LimitType : uint8_t {
    Quantity = 0,    // Limit on position size
    Notional = 1,    // Limit on position value
    Percentage = 2   // Limit as percentage of total
};

/**
 * @brief Position limit configuration
 */
struct PositionLimit {
    std::string identifier;        // Symbol, exchange, or "TOTAL"
    LimitType type{LimitType::Notional};
    double limit{0.0};             // The limit value
    double warningThreshold{0.8};  // Warn when usage exceeds this
    bool enabled{true};

    [[nodiscard]] double getWarningLevel() const noexcept {
        return limit * warningThreshold;
    }
};

/**
 * @brief Symbol-specific position limit
 */
struct SymbolPositionLimit : public PositionLimit {
    Symbol symbol;
    std::optional<double> maxLongQuantity;
    std::optional<double> maxShortQuantity;
    std::optional<double> maxNotional;
    std::optional<double> maxConcentrationPct; // Max % of portfolio
};

/**
 * @brief Exchange-specific position limit
 */
struct ExchangePositionLimit : public PositionLimit {
    Exchange exchange{Exchange::Unknown};
    double maxPositionValue{0.0};
    double maxOpenOrders{100};
    std::vector<std::string> allowedSymbols; // Empty = all allowed
    std::vector<std::string> blockedSymbols;
};

/**
 * @brief Portfolio-level limits
 */
struct PortfolioLimits {
    double maxTotalPositionValue{10000000.0};
    double maxGrossExposure{20000000.0};
    double maxNetExposure{5000000.0};
    double maxLeverage{5.0};
    size_t maxOpenPositions{100};
    double maxConcentrationPct{25.0}; // Max % in single position
};

/**
 * @brief Limit check result
 */
struct LimitCheckResult {
    bool passed{true};
    std::string limitName;
    double currentValue{0.0};
    double limitValue{0.0};
    double utilizationPct{0.0};
    std::string message;
};

/**
 * @brief Position Limits Manager
 *
 * Manages position limits with support for:
 * - Per-symbol limits
 * - Per-exchange limits
 * - Portfolio-level limits
 * - YAML configuration
 */
class PositionLimits {
public:
    /**
     * @brief Constructor
     */
    PositionLimits();

    /**
     * @brief Constructor with position manager
     */
    explicit PositionLimits(std::shared_ptr<PositionManager> positionManager);

    /**
     * @brief Destructor
     */
    ~PositionLimits();

    // Non-copyable
    PositionLimits(const PositionLimits&) = delete;
    PositionLimits& operator=(const PositionLimits&) = delete;

    // ===== Configuration =====

    /**
     * @brief Load limits from YAML file
     */
    bool loadFromYaml(const std::filesystem::path& configPath);

    /**
     * @brief Save limits to YAML file
     */
    bool saveToYaml(const std::filesystem::path& configPath) const;

    /**
     * @brief Load from YAML string
     */
    bool loadFromYamlString(const std::string& yamlContent);

    // ===== Symbol Limits =====

    /**
     * @brief Set symbol position limit
     */
    void setSymbolLimit(const Symbol& symbol, const SymbolPositionLimit& limit);

    /**
     * @brief Get symbol position limit
     */
    [[nodiscard]] std::optional<SymbolPositionLimit> getSymbolLimit(const Symbol& symbol) const;

    /**
     * @brief Remove symbol limit
     */
    void removeSymbolLimit(const Symbol& symbol);

    /**
     * @brief Get all symbol limits
     */
    [[nodiscard]] std::vector<SymbolPositionLimit> getAllSymbolLimits() const;

    // ===== Exchange Limits =====

    /**
     * @brief Set exchange position limit
     */
    void setExchangeLimit(Exchange exchange, const ExchangePositionLimit& limit);

    /**
     * @brief Get exchange position limit
     */
    [[nodiscard]] std::optional<ExchangePositionLimit> getExchangeLimit(Exchange exchange) const;

    /**
     * @brief Remove exchange limit
     */
    void removeExchangeLimit(Exchange exchange);

    /**
     * @brief Get all exchange limits
     */
    [[nodiscard]] std::vector<ExchangePositionLimit> getAllExchangeLimits() const;

    // ===== Portfolio Limits =====

    /**
     * @brief Set portfolio limits
     */
    void setPortfolioLimits(const PortfolioLimits& limits);

    /**
     * @brief Get portfolio limits
     */
    [[nodiscard]] const PortfolioLimits& getPortfolioLimits() const;

    // ===== Limit Checking =====

    /**
     * @brief Check if new position would exceed symbol limit
     */
    [[nodiscard]] LimitCheckResult checkSymbolLimit(
        const Symbol& symbol,
        double newQuantity,
        double price
    ) const;

    /**
     * @brief Check if new position would exceed exchange limit
     */
    [[nodiscard]] LimitCheckResult checkExchangeLimit(
        Exchange exchange,
        double additionalValue
    ) const;

    /**
     * @brief Check if new position would exceed portfolio limits
     */
    [[nodiscard]] LimitCheckResult checkPortfolioLimits(
        double additionalValue
    ) const;

    /**
     * @brief Check all limits for proposed trade
     */
    [[nodiscard]] std::vector<LimitCheckResult> checkAllLimits(
        const Symbol& symbol,
        Exchange exchange,
        Side side,
        double quantity,
        double price
    ) const;

    /**
     * @brief Check if symbol is allowed on exchange
     */
    [[nodiscard]] bool isSymbolAllowedOnExchange(
        const Symbol& symbol,
        Exchange exchange
    ) const;

    // ===== Utilization Queries =====

    /**
     * @brief Get current symbol limit utilization
     */
    [[nodiscard]] double getSymbolUtilization(const Symbol& symbol) const;

    /**
     * @brief Get current exchange limit utilization
     */
    [[nodiscard]] double getExchangeUtilization(Exchange exchange) const;

    /**
     * @brief Get current portfolio utilization
     */
    [[nodiscard]] double getPortfolioUtilization() const;

    /**
     * @brief Get all limit utilizations
     */
    [[nodiscard]] std::unordered_map<std::string, double> getAllUtilizations() const;

    // ===== Headroom Calculation =====

    /**
     * @brief Get remaining capacity for symbol
     */
    [[nodiscard]] double getSymbolHeadroom(const Symbol& symbol, Side side) const;

    /**
     * @brief Get remaining capacity for exchange
     */
    [[nodiscard]] double getExchangeHeadroom(Exchange exchange) const;

    /**
     * @brief Get remaining portfolio capacity
     */
    [[nodiscard]] double getPortfolioHeadroom() const;

    // ===== Position Manager Integration =====

    /**
     * @brief Set position manager for live utilization checks
     */
    void setPositionManager(std::shared_ptr<PositionManager> positionManager);

    /**
     * @brief Get position manager
     */
    [[nodiscard]] std::shared_ptr<PositionManager> getPositionManager() const;

private:
    std::shared_ptr<PositionManager> positionManager_;

    mutable std::shared_mutex mutex_;

    // Limits storage
    std::unordered_map<std::string, SymbolPositionLimit> symbolLimits_;
    std::unordered_map<Exchange, ExchangePositionLimit> exchangeLimits_;
    PortfolioLimits portfolioLimits_;

    // Default limits for symbols without specific config
    SymbolPositionLimit defaultSymbolLimit_;
    ExchangePositionLimit defaultExchangeLimit_;

    // Helper methods
    double getCurrentSymbolValue(const Symbol& symbol) const;
    double getCurrentExchangeValue(Exchange exchange) const;
    double getCurrentPortfolioValue() const;
};

/**
 * @brief Position limits builder
 */
class PositionLimitsBuilder {
public:
    PositionLimitsBuilder& withPositionManager(std::shared_ptr<PositionManager> pm);
    PositionLimitsBuilder& withSymbolLimit(const Symbol& symbol, double maxNotional);
    PositionLimitsBuilder& withExchangeLimit(Exchange exchange, double maxValue);
    PositionLimitsBuilder& withPortfolioLimits(const PortfolioLimits& limits);
    PositionLimitsBuilder& loadFromYaml(const std::filesystem::path& path);

    std::unique_ptr<PositionLimits> build();

private:
    std::shared_ptr<PositionManager> positionManager_;
    std::vector<std::pair<Symbol, SymbolPositionLimit>> symbolLimits_;
    std::vector<std::pair<Exchange, ExchangePositionLimit>> exchangeLimits_;
    PortfolioLimits portfolioLimits_;
    std::optional<std::filesystem::path> yamlPath_;
};

} // namespace hft::risk
