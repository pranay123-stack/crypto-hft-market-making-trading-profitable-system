#include "exchange/exchange_base.hpp"

#include <random>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace hft {
namespace exchange {

ExchangeBase::ExchangeBase(const ExchangeConfig& config)
    : config_(config) {}

ExchangeBase::~ExchangeBase() = default;

// ============================================================================
// Callback Invocation
// ============================================================================

void ExchangeBase::onOrderBook(const OrderBook& ob) {
    updateOrderBookCache(ob.symbol, ob);

    if (orderbook_callback_) {
        try {
            orderbook_callback_(ob);
        } catch (const std::exception& e) {
            onError("OrderBook callback exception: " + std::string(e.what()));
        }
    }
}

void ExchangeBase::onTrade(const Trade& trade) {
    if (trade_callback_) {
        try {
            trade_callback_(trade);
        } catch (const std::exception& e) {
            onError("Trade callback exception: " + std::string(e.what()));
        }
    }
}

void ExchangeBase::onTicker(const Ticker& ticker) {
    if (ticker_callback_) {
        try {
            ticker_callback_(ticker);
        } catch (const std::exception& e) {
            onError("Ticker callback exception: " + std::string(e.what()));
        }
    }
}

void ExchangeBase::onOrder(const Order& order) {
    if (order_callback_) {
        try {
            order_callback_(order);
        } catch (const std::exception& e) {
            onError("Order callback exception: " + std::string(e.what()));
        }
    }
}

void ExchangeBase::onPosition(const Position& position) {
    if (position_callback_) {
        try {
            position_callback_(position);
        } catch (const std::exception& e) {
            onError("Position callback exception: " + std::string(e.what()));
        }
    }
}

void ExchangeBase::onBalance(const Balance& balance) {
    if (balance_callback_) {
        try {
            balance_callback_(balance);
        } catch (const std::exception& e) {
            onError("Balance callback exception: " + std::string(e.what()));
        }
    }
}

void ExchangeBase::onConnectionStatus(ConnectionStatus status) {
    connection_status_ = status;

    if (connection_callback_) {
        try {
            connection_callback_(status);
        } catch (const std::exception& e) {
            onError("Connection callback exception: " + std::string(e.what()));
        }
    }
}

void ExchangeBase::onError(const std::string& error) {
    if (error_callback_) {
        try {
            error_callback_(error);
        } catch (...) {
            // Can't do much if error callback throws
        }
    }
}

// ============================================================================
// Cache Management
// ============================================================================

void ExchangeBase::updateOrderBookCache(const std::string& symbol, const OrderBook& ob) {
    std::lock_guard<std::mutex> lock(orderbook_mutex_);
    orderbook_cache_[symbol] = ob;
}

void ExchangeBase::clearOrderBookCache(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(orderbook_mutex_);
    orderbook_cache_.erase(symbol);
}

std::optional<OrderBook> ExchangeBase::getCachedOrderBook(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(orderbook_mutex_);
    auto it = orderbook_cache_.find(symbol);
    if (it != orderbook_cache_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::unordered_map<std::string, OrderBook> ExchangeBase::getAllCachedOrderBooks() const {
    std::lock_guard<std::mutex> lock(orderbook_mutex_);
    return orderbook_cache_;
}

void ExchangeBase::updateSymbolInfo(const SymbolInfo& info) {
    std::lock_guard<std::mutex> lock(symbol_info_mutex_);
    symbol_info_cache_[info.symbol] = info;
}

// ============================================================================
// Price/Quantity Rounding
// ============================================================================

double ExchangeBase::roundPrice(const std::string& symbol, double price) const {
    std::lock_guard<std::mutex> lock(symbol_info_mutex_);
    auto it = symbol_info_cache_.find(symbol);
    if (it == symbol_info_cache_.end() || it->second.tick_size == 0.0) {
        return price;
    }

    double tick_size = it->second.tick_size;
    return std::round(price / tick_size) * tick_size;
}

double ExchangeBase::roundQuantity(const std::string& symbol, double qty) const {
    std::lock_guard<std::mutex> lock(symbol_info_mutex_);
    auto it = symbol_info_cache_.find(symbol);
    if (it == symbol_info_cache_.end() || it->second.step_size == 0.0) {
        return qty;
    }

    double step_size = it->second.step_size;
    return std::floor(qty / step_size) * step_size;  // Always round down for safety
}

// ============================================================================
// Utility Methods
// ============================================================================

std::string ExchangeBase::generateClientOrderId() const {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;

    std::stringstream ss;
    ss << "HFT_" << std::hex << std::setfill('0') << std::setw(16) << dist(gen);
    return ss.str();
}

uint64_t ExchangeBase::currentTimeMs() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

uint64_t ExchangeBase::currentTimeNs() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

} // namespace exchange
} // namespace hft
