#pragma once

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <unordered_map>
#include <future>
#include <optional>

namespace hft {
namespace network {

// HTTP methods
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    PATCH
};

// HTTP response structure
struct HttpResponse {
    int status_code{0};
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string error;
    std::chrono::microseconds latency{0};
    bool success{false};

    nlohmann::json json() const {
        if (body.empty()) {
            return nlohmann::json{};
        }
        try {
            return nlohmann::json::parse(body);
        } catch (...) {
            return nlohmann::json{};
        }
    }
};

// Rate limit configuration
struct RateLimitConfig {
    uint32_t requests_per_second{10};
    uint32_t requests_per_minute{600};
    uint32_t burst_size{20};
    bool enable{true};
};

// Request signing configuration
struct SigningConfig {
    std::string api_key;
    std::string api_secret;
    std::string passphrase;  // For exchanges like OKX
    std::string sign_header{"X-MBX-SIGNATURE"};  // Header name for signature
    std::string key_header{"X-MBX-APIKEY"};      // Header name for API key
    std::string timestamp_header{"X-MBX-TIMESTAMP"};
    std::string timestamp_param{"timestamp"};
    std::string signature_param{"signature"};
    bool sign_body{false};       // Sign the request body
    bool sign_query{true};       // Sign query parameters
    bool add_timestamp{true};    // Add timestamp to request
    bool hmac_sha256{true};      // Use HMAC-SHA256 (vs other methods)
    bool base64_encode{false};   // Base64 encode signature
};

// REST client configuration
struct RestClientConfig {
    std::string base_url;
    std::string name;                           // Client identifier
    uint32_t connect_timeout_ms{5000};          // Connection timeout
    uint32_t request_timeout_ms{30000};         // Request timeout
    uint32_t max_retries{3};                    // Max retry attempts
    uint32_t retry_delay_ms{1000};              // Delay between retries
    size_t max_connections{10};                 // Max connections in pool
    bool keep_alive{true};                      // HTTP keep-alive
    bool verify_ssl{true};                      // Verify SSL certificates
    bool verbose{false};                        // Verbose logging
    RateLimitConfig rate_limit;
    SigningConfig signing;
    std::unordered_map<std::string, std::string> default_headers;
};

// Request structure
struct HttpRequest {
    HttpMethod method{HttpMethod::GET};
    std::string path;
    std::unordered_map<std::string, std::string> query_params;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool require_auth{false};
    int priority{0};  // Higher = higher priority
    uint32_t timeout_ms{0};  // 0 = use default
    std::function<void(const HttpResponse&)> callback;
};

// Rate limiter
class RateLimiter {
public:
    explicit RateLimiter(const RateLimitConfig& config);

    // Check if request can proceed (non-blocking)
    bool tryAcquire();

    // Wait until request can proceed (blocking)
    void acquire();

    // Get time until next available slot (microseconds)
    int64_t timeUntilAvailable() const;

    // Update configuration
    void updateConfig(const RateLimitConfig& config);

    // Get current usage stats
    uint32_t requestsLastSecond() const;
    uint32_t requestsLastMinute() const;

private:
    void pruneOldRequests();

    RateLimitConfig config_;
    std::deque<std::chrono::steady_clock::time_point> request_times_;
    mutable std::mutex mutex_;
};

// Request signer
class RequestSigner {
public:
    explicit RequestSigner(const SigningConfig& config);

    // Sign a request
    void sign(HttpRequest& request, const std::string& full_url);

    // Generate HMAC-SHA256 signature
    std::string hmacSha256(const std::string& key, const std::string& data);

    // Generate timestamp
    std::string generateTimestamp();

    // Update configuration
    void updateConfig(const SigningConfig& config);

    // Get API key
    const std::string& apiKey() const { return config_.api_key; }

private:
    SigningConfig config_;
};

// Async REST client with connection pooling
class RestClient : public std::enable_shared_from_this<RestClient> {
public:
    explicit RestClient(const RestClientConfig& config);
    ~RestClient();

    // Non-copyable
    RestClient(const RestClient&) = delete;
    RestClient& operator=(const RestClient&) = delete;

    // Synchronous requests
    HttpResponse get(const std::string& path,
                    const std::unordered_map<std::string, std::string>& params = {},
                    bool require_auth = false);

    HttpResponse post(const std::string& path,
                     const std::string& body = "",
                     bool require_auth = false);

    HttpResponse post(const std::string& path,
                     const nlohmann::json& body,
                     bool require_auth = false);

    HttpResponse put(const std::string& path,
                    const std::string& body = "",
                    bool require_auth = false);

    HttpResponse del(const std::string& path,
                    const std::unordered_map<std::string, std::string>& params = {},
                    bool require_auth = false);

    // Asynchronous requests
    std::future<HttpResponse> getAsync(const std::string& path,
                                       const std::unordered_map<std::string, std::string>& params = {},
                                       bool require_auth = false);

    std::future<HttpResponse> postAsync(const std::string& path,
                                        const std::string& body = "",
                                        bool require_auth = false);

    std::future<HttpResponse> postAsync(const std::string& path,
                                        const nlohmann::json& body,
                                        bool require_auth = false);

    // Generic request execution
    HttpResponse execute(HttpRequest& request);
    std::future<HttpResponse> executeAsync(HttpRequest request);

    // Queue a request with callback
    void queueRequest(HttpRequest request);

    // Configuration
    void updateConfig(const RestClientConfig& config);
    const RestClientConfig& config() const { return config_; }

    // Set custom signer
    void setSigner(std::shared_ptr<RequestSigner> signer) { signer_ = std::move(signer); }

    // Statistics
    uint64_t requestCount() const { return request_count_.load(); }
    uint64_t errorCount() const { return error_count_.load(); }
    double avgLatencyMs() const;

    // Start/stop async processing
    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

private:
    // CURL handle management
    CURL* acquireHandle();
    void releaseHandle(CURL* handle);
    void initHandle(CURL* handle);

    // Request execution
    HttpResponse executeImpl(HttpRequest& request, CURL* handle);

    // URL building
    std::string buildUrl(const std::string& path,
                        const std::unordered_map<std::string, std::string>& params);

    // Async worker thread
    void workerThread();

    // CURL callbacks
    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t headerCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

    // Configuration
    RestClientConfig config_;

    // CURL handle pool
    std::vector<CURL*> handle_pool_;
    std::mutex pool_mutex_;
    std::condition_variable pool_cv_;

    // Request queue for async processing
    std::priority_queue<HttpRequest,
                       std::vector<HttpRequest>,
                       std::function<bool(const HttpRequest&, const HttpRequest&)>> request_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Worker threads
    std::vector<std::thread> worker_threads_;
    std::atomic<bool> running_{false};

    // Rate limiter and signer
    std::unique_ptr<RateLimiter> rate_limiter_;
    std::shared_ptr<RequestSigner> signer_;

    // Statistics
    std::atomic<uint64_t> request_count_{0};
    std::atomic<uint64_t> error_count_{0};
    std::atomic<uint64_t> total_latency_us_{0};
};

// Factory function
std::shared_ptr<RestClient> createRestClient(const RestClientConfig& config);

} // namespace network
} // namespace hft
