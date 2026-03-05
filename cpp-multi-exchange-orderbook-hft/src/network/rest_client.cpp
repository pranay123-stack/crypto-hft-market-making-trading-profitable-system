#include "network/rest_client.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace hft {
namespace network {

// ============================================================================
// RateLimiter Implementation
// ============================================================================

RateLimiter::RateLimiter(const RateLimitConfig& config)
    : config_(config) {}

bool RateLimiter::tryAcquire() {
    if (!config_.enable) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pruneOldRequests();

    auto now = std::chrono::steady_clock::now();
    auto one_second_ago = now - std::chrono::seconds(1);
    auto one_minute_ago = now - std::chrono::minutes(1);

    // Count requests in last second
    size_t requests_last_second = std::count_if(
        request_times_.begin(), request_times_.end(),
        [one_second_ago](const auto& t) { return t > one_second_ago; }
    );

    // Count requests in last minute
    size_t requests_last_minute = std::count_if(
        request_times_.begin(), request_times_.end(),
        [one_minute_ago](const auto& t) { return t > one_minute_ago; }
    );

    if (requests_last_second >= config_.requests_per_second ||
        requests_last_minute >= config_.requests_per_minute) {
        return false;
    }

    request_times_.push_back(now);
    return true;
}

void RateLimiter::acquire() {
    while (!tryAcquire()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int64_t RateLimiter::timeUntilAvailable() const {
    if (!config_.enable) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (request_times_.empty()) {
        return 0;
    }

    auto now = std::chrono::steady_clock::now();
    auto one_second_ago = now - std::chrono::seconds(1);

    size_t requests_last_second = std::count_if(
        request_times_.begin(), request_times_.end(),
        [one_second_ago](const auto& t) { return t > one_second_ago; }
    );

    if (requests_last_second < config_.requests_per_second) {
        return 0;
    }

    // Find oldest request in the last second
    auto oldest_in_second = std::find_if(
        request_times_.begin(), request_times_.end(),
        [one_second_ago](const auto& t) { return t > one_second_ago; }
    );

    if (oldest_in_second != request_times_.end()) {
        auto wait_until = *oldest_in_second + std::chrono::seconds(1);
        auto wait_duration = std::chrono::duration_cast<std::chrono::microseconds>(
            wait_until - now
        );
        return std::max(int64_t(0), wait_duration.count());
    }

    return 0;
}

void RateLimiter::updateConfig(const RateLimitConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

uint32_t RateLimiter::requestsLastSecond() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto one_second_ago = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    return static_cast<uint32_t>(std::count_if(
        request_times_.begin(), request_times_.end(),
        [one_second_ago](const auto& t) { return t > one_second_ago; }
    ));
}

uint32_t RateLimiter::requestsLastMinute() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto one_minute_ago = std::chrono::steady_clock::now() - std::chrono::minutes(1);
    return static_cast<uint32_t>(std::count_if(
        request_times_.begin(), request_times_.end(),
        [one_minute_ago](const auto& t) { return t > one_minute_ago; }
    ));
}

void RateLimiter::pruneOldRequests() {
    auto one_minute_ago = std::chrono::steady_clock::now() - std::chrono::minutes(1);
    while (!request_times_.empty() && request_times_.front() < one_minute_ago) {
        request_times_.pop_front();
    }
}

// ============================================================================
// RequestSigner Implementation
// ============================================================================

RequestSigner::RequestSigner(const SigningConfig& config)
    : config_(config) {}

void RequestSigner::sign(HttpRequest& request, const std::string& full_url) {
    if (config_.api_key.empty() || config_.api_secret.empty()) {
        return;
    }

    // Add API key header
    request.headers[config_.key_header] = config_.api_key;

    // Build signature payload
    std::string signature_payload;

    if (config_.add_timestamp) {
        std::string timestamp = generateTimestamp();
        request.query_params[config_.timestamp_param] = timestamp;
        request.headers[config_.timestamp_header] = timestamp;
    }

    // Build query string for signing
    if (config_.sign_query && !request.query_params.empty()) {
        std::vector<std::pair<std::string, std::string>> sorted_params(
            request.query_params.begin(), request.query_params.end()
        );
        std::sort(sorted_params.begin(), sorted_params.end());

        for (const auto& [key, value] : sorted_params) {
            if (!signature_payload.empty()) {
                signature_payload += "&";
            }
            signature_payload += key + "=" + value;
        }
    }

    // Add body to signature if required
    if (config_.sign_body && !request.body.empty()) {
        if (!signature_payload.empty()) {
            signature_payload += "&";
        }
        signature_payload += request.body;
    }

    // Generate signature
    std::string signature;
    if (config_.hmac_sha256) {
        signature = hmacSha256(config_.api_secret, signature_payload);
    }

    if (config_.base64_encode) {
        // Base64 encode is typically done inside hmacSha256 for certain exchanges
    }

    // Add signature to request
    request.query_params[config_.signature_param] = signature;
    request.headers[config_.sign_header] = signature;
}

std::string RequestSigner::hmacSha256(const std::string& key, const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         key.c_str(), static_cast<int>(key.length()),
         reinterpret_cast<const unsigned char*>(data.c_str()),
         data.length(),
         hash, &hash_len);

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(hash[i]);
    }

    return ss.str();
}

std::string RequestSigner::generateTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    return std::to_string(ms);
}

void RequestSigner::updateConfig(const SigningConfig& config) {
    config_ = config;
}

// ============================================================================
// RestClient Implementation
// ============================================================================

RestClient::RestClient(const RestClientConfig& config)
    : config_(config),
      request_queue_([](const HttpRequest& a, const HttpRequest& b) {
          return a.priority < b.priority;  // Higher priority first
      }) {

    // Initialize CURL globally (thread-safe)
    static std::once_flag curl_init_flag;
    std::call_once(curl_init_flag, []() {
        curl_global_init(CURL_GLOBAL_ALL);
    });

    // Create rate limiter
    rate_limiter_ = std::make_unique<RateLimiter>(config_.rate_limit);

    // Create signer
    signer_ = std::make_shared<RequestSigner>(config_.signing);

    // Pre-populate handle pool
    for (size_t i = 0; i < config_.max_connections; ++i) {
        CURL* handle = curl_easy_init();
        if (handle) {
            initHandle(handle);
            handle_pool_.push_back(handle);
        }
    }
}

RestClient::~RestClient() {
    stop();

    // Clean up CURL handles
    std::lock_guard<std::mutex> lock(pool_mutex_);
    for (CURL* handle : handle_pool_) {
        curl_easy_cleanup(handle);
    }
    handle_pool_.clear();
}

void RestClient::start() {
    if (running_.load()) {
        return;
    }

    running_ = true;

    // Start worker threads
    size_t num_workers = std::min(config_.max_connections, size_t(4));
    for (size_t i = 0; i < num_workers; ++i) {
        worker_threads_.emplace_back([this]() {
            this->workerThread();
        });
    }
}

void RestClient::stop() {
    running_ = false;

    // Wake up all worker threads
    queue_cv_.notify_all();
    pool_cv_.notify_all();

    // Join worker threads
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();
}

CURL* RestClient::acquireHandle() {
    std::unique_lock<std::mutex> lock(pool_mutex_);

    pool_cv_.wait(lock, [this]() {
        return !handle_pool_.empty() || !running_.load();
    });

    if (!running_.load() || handle_pool_.empty()) {
        return nullptr;
    }

    CURL* handle = handle_pool_.back();
    handle_pool_.pop_back();
    return handle;
}

void RestClient::releaseHandle(CURL* handle) {
    if (!handle) return;

    // Reset handle for reuse
    curl_easy_reset(handle);
    initHandle(handle);

    std::lock_guard<std::mutex> lock(pool_mutex_);
    handle_pool_.push_back(handle);
    pool_cv_.notify_one();
}

void RestClient::initHandle(CURL* handle) {
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, config_.keep_alive ? 1L : 0L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, 60L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(config_.connect_timeout_ms));
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(config_.request_timeout_ms));
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, config_.verify_ssl ? 1L : 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, config_.verify_ssl ? 2L : 0L);
    curl_easy_setopt(handle, CURLOPT_VERBOSE, config_.verbose ? 1L : 0L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
}

std::string RestClient::buildUrl(const std::string& path,
                                 const std::unordered_map<std::string, std::string>& params) {
    std::string url = config_.base_url;

    // Handle path
    if (!path.empty()) {
        if (url.back() != '/' && path.front() != '/') {
            url += '/';
        } else if (url.back() == '/' && path.front() == '/') {
            url.pop_back();
        }
        url += path;
    }

    // Add query parameters
    if (!params.empty()) {
        url += '?';
        bool first = true;
        for (const auto& [key, value] : params) {
            if (!first) url += '&';
            first = false;

            // URL encode key and value
            char* encoded_key = curl_easy_escape(nullptr, key.c_str(), static_cast<int>(key.length()));
            char* encoded_value = curl_easy_escape(nullptr, value.c_str(), static_cast<int>(value.length()));

            if (encoded_key && encoded_value) {
                url += encoded_key;
                url += '=';
                url += encoded_value;
            }

            curl_free(encoded_key);
            curl_free(encoded_value);
        }
    }

    return url;
}

size_t RestClient::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* response = static_cast<std::string*>(userdata);
    size_t total_size = size * nmemb;
    response->append(ptr, total_size);
    return total_size;
}

size_t RestClient::headerCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* headers = static_cast<std::unordered_map<std::string, std::string>*>(userdata);
    size_t total_size = size * nmemb;

    std::string header(ptr, total_size);
    size_t colon_pos = header.find(':');
    if (colon_pos != std::string::npos) {
        std::string key = header.substr(0, colon_pos);
        std::string value = header.substr(colon_pos + 1);

        // Trim whitespace
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(key);
        trim(value);

        (*headers)[key] = value;
    }

    return total_size;
}

HttpResponse RestClient::executeImpl(HttpRequest& request, CURL* handle) {
    HttpResponse response;
    auto start_time = std::chrono::steady_clock::now();

    // Rate limiting
    rate_limiter_->acquire();

    // Sign request if needed
    std::string url = buildUrl(request.path, request.query_params);
    if (request.require_auth && signer_) {
        signer_->sign(request, url);
        // Rebuild URL with signature params
        url = buildUrl(request.path, request.query_params);
    }

    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());

    // Set method
    switch (request.method) {
        case HttpMethod::GET:
            curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
            break;
        case HttpMethod::POST:
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
            break;
        case HttpMethod::PUT:
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PUT");
            break;
        case HttpMethod::DELETE:
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
            break;
        case HttpMethod::PATCH:
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PATCH");
            break;
    }

    // Set body
    if (!request.body.empty()) {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, request.body.length());
    }

    // Set headers
    struct curl_slist* headers = nullptr;

    // Add default headers
    for (const auto& [key, value] : config_.default_headers) {
        std::string header = key + ": " + value;
        headers = curl_slist_append(headers, header.c_str());
    }

    // Add request headers
    for (const auto& [key, value] : request.headers) {
        std::string header = key + ": " + value;
        headers = curl_slist_append(headers, header.c_str());
    }

    // Default content type for POST
    if (request.method == HttpMethod::POST && !request.body.empty()) {
        bool has_content_type = false;
        for (const auto& [key, value] : request.headers) {
            if (key == "Content-Type") {
                has_content_type = true;
                break;
            }
        }
        if (!has_content_type) {
            headers = curl_slist_append(headers, "Content-Type: application/json");
        }
    }

    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

    // Set callbacks
    std::string response_body;
    std::unordered_map<std::string, std::string> response_headers;

    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response_headers);

    // Custom timeout
    if (request.timeout_ms > 0) {
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS,
                        static_cast<long>(request.timeout_ms));
    }

    // Execute with retries
    CURLcode res = CURLE_OK;
    for (uint32_t attempt = 0; attempt <= config_.max_retries; ++attempt) {
        res = curl_easy_perform(handle);

        if (res == CURLE_OK) {
            break;
        }

        if (attempt < config_.max_retries) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.retry_delay_ms)
            );
            response_body.clear();
            response_headers.clear();
        }
    }

    // Clean up headers
    curl_slist_free_all(headers);

    // Calculate latency
    auto end_time = std::chrono::steady_clock::now();
    response.latency = std::chrono::duration_cast<std::chrono::microseconds>(
        end_time - start_time
    );

    // Fill response
    if (res == CURLE_OK) {
        long status_code;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status_code);
        response.status_code = static_cast<int>(status_code);
        response.body = std::move(response_body);
        response.headers = std::move(response_headers);
        response.success = (status_code >= 200 && status_code < 300);
    } else {
        response.error = curl_easy_strerror(res);
        response.success = false;
        error_count_++;
    }

    // Update statistics
    request_count_++;
    total_latency_us_ += response.latency.count();

    return response;
}

HttpResponse RestClient::execute(HttpRequest& request) {
    CURL* handle = acquireHandle();
    if (!handle) {
        HttpResponse response;
        response.error = "Failed to acquire CURL handle";
        return response;
    }

    HttpResponse response = executeImpl(request, handle);
    releaseHandle(handle);

    // Execute callback if set
    if (request.callback) {
        request.callback(response);
    }

    return response;
}

std::future<HttpResponse> RestClient::executeAsync(HttpRequest request) {
    auto promise = std::make_shared<std::promise<HttpResponse>>();
    auto future = promise->get_future();

    request.callback = [promise](const HttpResponse& response) {
        promise->set_value(response);
    };

    queueRequest(std::move(request));

    return future;
}

void RestClient::queueRequest(HttpRequest request) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        request_queue_.push(std::move(request));
    }
    queue_cv_.notify_one();
}

HttpResponse RestClient::get(const std::string& path,
                             const std::unordered_map<std::string, std::string>& params,
                             bool require_auth) {
    HttpRequest request;
    request.method = HttpMethod::GET;
    request.path = path;
    request.query_params = params;
    request.require_auth = require_auth;
    return execute(request);
}

HttpResponse RestClient::post(const std::string& path,
                              const std::string& body,
                              bool require_auth) {
    HttpRequest request;
    request.method = HttpMethod::POST;
    request.path = path;
    request.body = body;
    request.require_auth = require_auth;
    return execute(request);
}

HttpResponse RestClient::post(const std::string& path,
                              const nlohmann::json& body,
                              bool require_auth) {
    return post(path, body.dump(), require_auth);
}

HttpResponse RestClient::put(const std::string& path,
                             const std::string& body,
                             bool require_auth) {
    HttpRequest request;
    request.method = HttpMethod::PUT;
    request.path = path;
    request.body = body;
    request.require_auth = require_auth;
    return execute(request);
}

HttpResponse RestClient::del(const std::string& path,
                             const std::unordered_map<std::string, std::string>& params,
                             bool require_auth) {
    HttpRequest request;
    request.method = HttpMethod::DELETE;
    request.path = path;
    request.query_params = params;
    request.require_auth = require_auth;
    return execute(request);
}

std::future<HttpResponse> RestClient::getAsync(const std::string& path,
                                               const std::unordered_map<std::string, std::string>& params,
                                               bool require_auth) {
    HttpRequest request;
    request.method = HttpMethod::GET;
    request.path = path;
    request.query_params = params;
    request.require_auth = require_auth;
    return executeAsync(std::move(request));
}

std::future<HttpResponse> RestClient::postAsync(const std::string& path,
                                                const std::string& body,
                                                bool require_auth) {
    HttpRequest request;
    request.method = HttpMethod::POST;
    request.path = path;
    request.body = body;
    request.require_auth = require_auth;
    return executeAsync(std::move(request));
}

std::future<HttpResponse> RestClient::postAsync(const std::string& path,
                                                const nlohmann::json& body,
                                                bool require_auth) {
    return postAsync(path, body.dump(), require_auth);
}

void RestClient::updateConfig(const RestClientConfig& config) {
    config_ = config;
    rate_limiter_->updateConfig(config_.rate_limit);
    signer_->updateConfig(config_.signing);
}

double RestClient::avgLatencyMs() const {
    uint64_t count = request_count_.load();
    if (count == 0) return 0.0;
    return static_cast<double>(total_latency_us_.load()) / count / 1000.0;
}

void RestClient::workerThread() {
    while (running_.load()) {
        HttpRequest request;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() {
                return !request_queue_.empty() || !running_.load();
            });

            if (!running_.load() && request_queue_.empty()) {
                break;
            }

            if (request_queue_.empty()) {
                continue;
            }

            request = std::move(const_cast<HttpRequest&>(request_queue_.top()));
            request_queue_.pop();
        }

        // Execute request
        execute(request);
    }
}

std::shared_ptr<RestClient> createRestClient(const RestClientConfig& config) {
    return std::make_shared<RestClient>(config);
}

} // namespace network
} // namespace hft
