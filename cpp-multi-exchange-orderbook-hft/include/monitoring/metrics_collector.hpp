#pragma once

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>
#include <sstream>
#include <cmath>

namespace hft {
namespace monitoring {

// ============================================================================
// Types
// ============================================================================

using Labels = std::unordered_map<std::string, std::string>;

// ============================================================================
// Metric Types
// ============================================================================

enum class MetricType : uint8_t {
    Counter,
    Gauge,
    Histogram,
    Summary
};

// ============================================================================
// Counter - Monotonically increasing value
// ============================================================================

class Counter {
public:
    Counter(const std::string& name, const std::string& help, const Labels& labels = {})
        : name_(name), help_(help), labels_(labels), value_(0.0) {}

    void increment(double value = 1.0) {
        double current = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(current, current + value,
               std::memory_order_release, std::memory_order_relaxed)) {}
    }

    double get() const { return value_.load(std::memory_order_acquire); }
    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }
    const Labels& labels() const { return labels_; }

private:
    std::string name_;
    std::string help_;
    Labels labels_;
    std::atomic<double> value_;
};

// ============================================================================
// Gauge - Value that can go up and down
// ============================================================================

class Gauge {
public:
    Gauge(const std::string& name, const std::string& help, const Labels& labels = {})
        : name_(name), help_(help), labels_(labels), value_(0.0) {}

    void set(double value) { value_.store(value, std::memory_order_release); }
    void increment(double value = 1.0) {
        double current = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(current, current + value,
               std::memory_order_release, std::memory_order_relaxed)) {}
    }
    void decrement(double value = 1.0) { increment(-value); }

    double get() const { return value_.load(std::memory_order_acquire); }
    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }
    const Labels& labels() const { return labels_; }

private:
    std::string name_;
    std::string help_;
    Labels labels_;
    std::atomic<double> value_;
};

// ============================================================================
// Histogram - Distribution of values in buckets
// ============================================================================

class Histogram {
public:
    Histogram(const std::string& name, const std::string& help,
              const std::vector<double>& buckets, const Labels& labels = {})
        : name_(name), help_(help), labels_(labels), buckets_(buckets),
          bucket_counts_(buckets.size() + 1), sum_(0.0), count_(0) {
        for (auto& bc : bucket_counts_) {
            bc.store(0, std::memory_order_relaxed);
        }
    }

    void observe(double value) {
        // Find bucket and increment
        for (size_t i = 0; i < buckets_.size(); ++i) {
            if (value <= buckets_[i]) {
                bucket_counts_[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        // Always increment +Inf bucket
        bucket_counts_.back().fetch_add(1, std::memory_order_relaxed);

        // Update sum and count
        double current_sum = sum_.load(std::memory_order_relaxed);
        while (!sum_.compare_exchange_weak(current_sum, current_sum + value,
               std::memory_order_release, std::memory_order_relaxed)) {}
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }
    const Labels& labels() const { return labels_; }
    const std::vector<double>& buckets() const { return buckets_; }

    uint64_t getBucketCount(size_t idx) const {
        return bucket_counts_[idx].load(std::memory_order_acquire);
    }
    double getSum() const { return sum_.load(std::memory_order_acquire); }
    uint64_t getCount() const { return count_.load(std::memory_order_acquire); }

private:
    std::string name_;
    std::string help_;
    Labels labels_;
    std::vector<double> buckets_;
    std::vector<std::atomic<uint64_t>> bucket_counts_;
    std::atomic<double> sum_;
    std::atomic<uint64_t> count_;
};

// ============================================================================
// MetricsCollector - Central metrics registry (Singleton)
// ============================================================================

class MetricsCollector {
public:
    static MetricsCollector& instance() {
        static MetricsCollector instance;
        return instance;
    }

    // Disable copy
    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;

    // ========================================================================
    // Counter operations
    // ========================================================================

    Counter& getCounter(const std::string& name, const std::string& help = "",
                        const Labels& labels = {}) {
        std::string key = makeKey(name, labels);
        std::unique_lock<std::shared_mutex> lock(mutex_);

        auto it = counters_.find(key);
        if (it == counters_.end()) {
            counters_[key] = std::make_unique<Counter>(name, help, labels);
            it = counters_.find(key);
        }
        return *it->second;
    }

    void incrementCounter(const std::string& name, double value = 1.0,
                          const Labels& labels = {}) {
        getCounter(name, "", labels).increment(value);
    }

    // ========================================================================
    // Gauge operations
    // ========================================================================

    Gauge& getGauge(const std::string& name, const std::string& help = "",
                    const Labels& labels = {}) {
        std::string key = makeKey(name, labels);
        std::unique_lock<std::shared_mutex> lock(mutex_);

        auto it = gauges_.find(key);
        if (it == gauges_.end()) {
            gauges_[key] = std::make_unique<Gauge>(name, help, labels);
            it = gauges_.find(key);
        }
        return *it->second;
    }

    void setGauge(const std::string& name, double value, const Labels& labels = {}) {
        getGauge(name, "", labels).set(value);
    }

    // ========================================================================
    // Histogram operations
    // ========================================================================

    Histogram& getHistogram(const std::string& name, const std::string& help = "",
                            const std::vector<double>& buckets = defaultLatencyBuckets(),
                            const Labels& labels = {}) {
        std::string key = makeKey(name, labels);
        std::unique_lock<std::shared_mutex> lock(mutex_);

        auto it = histograms_.find(key);
        if (it == histograms_.end()) {
            histograms_[key] = std::make_unique<Histogram>(name, help, buckets, labels);
            it = histograms_.find(key);
        }
        return *it->second;
    }

    void observeHistogram(const std::string& name, double value,
                          const Labels& labels = {}) {
        getHistogram(name, "", defaultLatencyBuckets(), labels).observe(value);
    }

    // ========================================================================
    // Default buckets
    // ========================================================================

    static std::vector<double> defaultLatencyBuckets() {
        // Microseconds: 1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000
        return {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
    }

    static std::vector<double> defaultSizeBuckets() {
        // Bytes: 100, 1K, 10K, 100K, 1M, 10M
        return {100, 1024, 10240, 102400, 1048576, 10485760};
    }

    // ========================================================================
    // Prometheus text format output
    // ========================================================================

    std::string getMetricsText() const {
        std::ostringstream oss;
        std::shared_lock<std::shared_mutex> lock(mutex_);

        // Output counters
        std::unordered_map<std::string, bool> printed_help;

        for (const auto& [key, counter] : counters_) {
            if (!printed_help[counter->name()]) {
                if (!counter->help().empty()) {
                    oss << "# HELP " << counter->name() << " " << counter->help() << "\n";
                }
                oss << "# TYPE " << counter->name() << " counter\n";
                printed_help[counter->name()] = true;
            }
            oss << counter->name() << formatLabels(counter->labels())
                << " " << counter->get() << "\n";
        }

        // Output gauges
        printed_help.clear();
        for (const auto& [key, gauge] : gauges_) {
            if (!printed_help[gauge->name()]) {
                if (!gauge->help().empty()) {
                    oss << "# HELP " << gauge->name() << " " << gauge->help() << "\n";
                }
                oss << "# TYPE " << gauge->name() << " gauge\n";
                printed_help[gauge->name()] = true;
            }
            oss << gauge->name() << formatLabels(gauge->labels())
                << " " << gauge->get() << "\n";
        }

        // Output histograms
        printed_help.clear();
        for (const auto& [key, histogram] : histograms_) {
            if (!printed_help[histogram->name()]) {
                if (!histogram->help().empty()) {
                    oss << "# HELP " << histogram->name() << " " << histogram->help() << "\n";
                }
                oss << "# TYPE " << histogram->name() << " histogram\n";
                printed_help[histogram->name()] = true;
            }

            const auto& buckets = histogram->buckets();
            uint64_t cumulative = 0;

            for (size_t i = 0; i < buckets.size(); ++i) {
                cumulative += histogram->getBucketCount(i);
                Labels bucket_labels = histogram->labels();
                bucket_labels["le"] = formatDouble(buckets[i]);
                oss << histogram->name() << "_bucket" << formatLabels(bucket_labels)
                    << " " << cumulative << "\n";
            }

            // +Inf bucket
            Labels inf_labels = histogram->labels();
            inf_labels["le"] = "+Inf";
            oss << histogram->name() << "_bucket" << formatLabels(inf_labels)
                << " " << histogram->getCount() << "\n";

            // Sum and count
            oss << histogram->name() << "_sum" << formatLabels(histogram->labels())
                << " " << histogram->getSum() << "\n";
            oss << histogram->name() << "_count" << formatLabels(histogram->labels())
                << " " << histogram->getCount() << "\n";
        }

        return oss.str();
    }

    // ========================================================================
    // HTTP Server for Prometheus scraping
    // ========================================================================

    void startHttpServer(uint16_t port = 9090) {
        if (http_running_.load(std::memory_order_acquire)) {
            return;
        }

        http_port_ = port;
        http_running_.store(true, std::memory_order_release);
        http_thread_ = std::thread(&MetricsCollector::httpServerLoop, this);
    }

    void stopHttpServer() {
        http_running_.store(false, std::memory_order_release);
        if (http_thread_.joinable()) {
            http_thread_.join();
        }
    }

    // ========================================================================
    // Clear all metrics
    // ========================================================================

    void clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        counters_.clear();
        gauges_.clear();
        histograms_.clear();
    }

private:
    MetricsCollector() = default;

    std::string makeKey(const std::string& name, const Labels& labels) const {
        std::string key = name;
        for (const auto& [k, v] : labels) {
            key += ";" + k + "=" + v;
        }
        return key;
    }

    static std::string formatLabels(const Labels& labels) {
        if (labels.empty()) {
            return "";
        }
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& [k, v] : labels) {
            if (!first) oss << ",";
            oss << k << "=\"" << escapeLabel(v) << "\"";
            first = false;
        }
        oss << "}";
        return oss.str();
    }

    static std::string escapeLabel(const std::string& value) {
        std::string escaped;
        for (char c : value) {
            if (c == '\\') escaped += "\\\\";
            else if (c == '"') escaped += "\\\"";
            else if (c == '\n') escaped += "\\n";
            else escaped += c;
        }
        return escaped;
    }

    static std::string formatDouble(double value) {
        if (std::isinf(value)) {
            return value > 0 ? "+Inf" : "-Inf";
        }
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }

    void httpServerLoop();  // Implemented in .cpp

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Counter>> counters_;
    std::unordered_map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::unordered_map<std::string, std::unique_ptr<Histogram>> histograms_;

    std::thread http_thread_;
    std::atomic<bool> http_running_{false};
    uint16_t http_port_{9090};
};

// ============================================================================
// Helper Macros
// ============================================================================

#define METRIC_COUNTER_INC(name, ...) \
    ::hft::monitoring::MetricsCollector::instance().incrementCounter(name, 1.0, {__VA_ARGS__})

#define METRIC_COUNTER_ADD(name, value, ...) \
    ::hft::monitoring::MetricsCollector::instance().incrementCounter(name, value, {__VA_ARGS__})

#define METRIC_GAUGE_SET(name, value, ...) \
    ::hft::monitoring::MetricsCollector::instance().setGauge(name, value, {__VA_ARGS__})

#define METRIC_HISTOGRAM_OBSERVE(name, value, ...) \
    ::hft::monitoring::MetricsCollector::instance().observeHistogram(name, value, {__VA_ARGS__})

// Latency measurement helper
#define METRIC_LATENCY_START() \
    auto _metric_start_time_ = std::chrono::high_resolution_clock::now()

#define METRIC_LATENCY_END(name, ...) \
    do { \
        auto _metric_end_time_ = std::chrono::high_resolution_clock::now(); \
        auto _metric_duration_ = std::chrono::duration_cast<std::chrono::microseconds>( \
            _metric_end_time_ - _metric_start_time_).count(); \
        ::hft::monitoring::MetricsCollector::instance().observeHistogram( \
            name, static_cast<double>(_metric_duration_), {__VA_ARGS__}); \
    } while(0)

// ============================================================================
// Pre-defined HFT Metrics
// ============================================================================

namespace hft_metrics {

// Trading metrics
inline void orderSent(const std::string& exchange, const std::string& side,
                      const std::string& type) {
    MetricsCollector::instance().incrementCounter(
        "hft_orders_total", 1.0,
        {{"exchange", exchange}, {"side", side}, {"type", type}});
}

inline void orderRejected(const std::string& exchange, const std::string& reason) {
    MetricsCollector::instance().incrementCounter(
        "hft_orders_rejected_total", 1.0,
        {{"exchange", exchange}, {"reason", reason}});
}

inline void orderFilled(const std::string& exchange, const std::string& side) {
    MetricsCollector::instance().incrementCounter(
        "hft_fills_total", 1.0,
        {{"exchange", exchange}, {"side", side}});
}

inline void setPosition(const std::string& exchange, const std::string& symbol,
                        double quantity) {
    MetricsCollector::instance().setGauge(
        "hft_position_quantity", quantity,
        {{"exchange", exchange}, {"symbol", symbol}});
}

inline void setPnL(const std::string& exchange, const std::string& symbol,
                   const std::string& type, double value) {
    MetricsCollector::instance().setGauge(
        "hft_position_pnl", value,
        {{"exchange", exchange}, {"symbol", symbol}, {"type", type}});
}

// Latency metrics
inline void recordOrderLatency(const std::string& exchange, const std::string& type,
                               double latency_us) {
    MetricsCollector::instance().observeHistogram(
        "hft_order_latency_us", latency_us,
        {{"exchange", exchange}, {"type", type}});
}

inline void recordMarketDataLatency(const std::string& exchange, double latency_us) {
    MetricsCollector::instance().observeHistogram(
        "hft_market_data_latency_us", latency_us,
        {{"exchange", exchange}});
}

inline void recordStrategyLatency(const std::string& strategy, double latency_us) {
    MetricsCollector::instance().observeHistogram(
        "hft_strategy_latency_us", latency_us,
        {{"strategy", strategy}});
}

// Connectivity metrics
inline void setWebSocketConnected(const std::string& exchange, bool connected) {
    MetricsCollector::instance().setGauge(
        "hft_websocket_connected", connected ? 1.0 : 0.0,
        {{"exchange", exchange}});
}

inline void webSocketReconnect(const std::string& exchange) {
    MetricsCollector::instance().incrementCounter(
        "hft_websocket_reconnects_total", 1.0,
        {{"exchange", exchange}});
}

inline void setOrderbookStaleness(const std::string& exchange, const std::string& symbol,
                                  double staleness_ms) {
    MetricsCollector::instance().setGauge(
        "hft_orderbook_staleness_ms", staleness_ms,
        {{"exchange", exchange}, {"symbol", symbol}});
}

// Risk metrics
inline void setExposure(double exposure) {
    MetricsCollector::instance().setGauge("hft_exposure_total", exposure, {});
}

inline void setDailyPnL(double pnl) {
    MetricsCollector::instance().setGauge("hft_daily_pnl", pnl, {});
}

inline void setDrawdown(double drawdown) {
    MetricsCollector::instance().setGauge("hft_drawdown", drawdown, {});
}

inline void circuitBreakerTriggered(const std::string& reason) {
    MetricsCollector::instance().incrementCounter(
        "hft_circuit_breaker_triggered_total", 1.0,
        {{"reason", reason}});
}

} // namespace hft_metrics

} // namespace monitoring
} // namespace hft
