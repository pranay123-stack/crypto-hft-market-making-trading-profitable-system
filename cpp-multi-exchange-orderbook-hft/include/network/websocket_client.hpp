#pragma once

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
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

namespace hft {
namespace network {

// WebSocket connection state
enum class WebSocketState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Failed,
    Closing
};

// WebSocket configuration
struct WebSocketConfig {
    std::string url;
    std::string name;                           // Connection identifier
    uint32_t reconnect_initial_delay_ms{1000};  // Initial reconnect delay
    uint32_t reconnect_max_delay_ms{30000};     // Maximum reconnect delay
    double reconnect_backoff_multiplier{2.0};   // Exponential backoff multiplier
    uint32_t max_reconnect_attempts{0};         // 0 = unlimited
    uint32_t ping_interval_ms{30000};           // Ping interval
    uint32_t pong_timeout_ms{10000};            // Pong timeout
    uint32_t connect_timeout_ms{10000};         // Connection timeout
    size_t max_message_queue_size{10000};       // Maximum queued messages
    bool auto_reconnect{true};                  // Auto-reconnect on disconnect
    bool enable_compression{true};              // Enable permessage-deflate
    std::unordered_map<std::string, std::string> headers;  // Custom headers
};

// Message types
enum class MessageType {
    Text,
    Binary,
    Ping,
    Pong,
    Close
};

// Callback types
using OnOpenCallback = std::function<void()>;
using OnCloseCallback = std::function<void(int code, const std::string& reason)>;
using OnMessageCallback = std::function<void(const std::string& message, MessageType type)>;
using OnErrorCallback = std::function<void(const std::string& error)>;
using OnReconnectCallback = std::function<void(uint32_t attempt)>;
using OnStateChangeCallback = std::function<void(WebSocketState state)>;

// WebSocket client using websocketpp
class WebSocketClient : public std::enable_shared_from_this<WebSocketClient> {
public:
    using Client = websocketpp::client<websocketpp::config::asio_tls_client>;
    using ConnectionHandle = websocketpp::connection_hdl;
    using ContextPtr = std::shared_ptr<boost::asio::ssl::context>;

    explicit WebSocketClient(const WebSocketConfig& config);
    ~WebSocketClient();

    // Non-copyable, movable
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    WebSocketClient(WebSocketClient&&) = default;
    WebSocketClient& operator=(WebSocketClient&&) = default;

    // Connection management
    bool connect();
    void disconnect(int code = websocketpp::close::status::normal,
                   const std::string& reason = "Client disconnect");
    void reconnect();

    // Message sending
    bool send(const std::string& message, MessageType type = MessageType::Text);
    bool send(const nlohmann::json& json_message);
    bool sendBinary(const std::vector<uint8_t>& data);

    // Queue management
    size_t queueSize() const;
    void clearQueue();

    // State queries
    WebSocketState state() const { return state_.load(); }
    bool isConnected() const { return state_.load() == WebSocketState::Connected; }
    std::string url() const { return config_.url; }
    std::string name() const { return config_.name; }

    // Statistics
    uint64_t messagesSent() const { return messages_sent_.load(); }
    uint64_t messagesReceived() const { return messages_received_.load(); }
    uint64_t bytessSent() const { return bytes_sent_.load(); }
    uint64_t bytesReceived() const { return bytes_received_.load(); }
    uint32_t reconnectAttempts() const { return reconnect_attempts_.load(); }
    std::chrono::steady_clock::time_point lastMessageTime() const;

    // Callback setters
    void setOnOpen(OnOpenCallback callback) { on_open_ = std::move(callback); }
    void setOnClose(OnCloseCallback callback) { on_close_ = std::move(callback); }
    void setOnMessage(OnMessageCallback callback) { on_message_ = std::move(callback); }
    void setOnError(OnErrorCallback callback) { on_error_ = std::move(callback); }
    void setOnReconnect(OnReconnectCallback callback) { on_reconnect_ = std::move(callback); }
    void setOnStateChange(OnStateChangeCallback callback) { on_state_change_ = std::move(callback); }

    // Configuration update
    void updateConfig(const WebSocketConfig& config);
    const WebSocketConfig& config() const { return config_; }

private:
    // WebSocket event handlers
    void onOpen(ConnectionHandle hdl);
    void onClose(ConnectionHandle hdl);
    void onMessage(ConnectionHandle hdl, Client::message_ptr msg);
    void onFail(ConnectionHandle hdl);
    bool onPing(ConnectionHandle hdl, const std::string& payload);
    void onPong(ConnectionHandle hdl, const std::string& payload);
    void onPongTimeout(ConnectionHandle hdl, const std::string& payload);

    // TLS initialization
    ContextPtr onTlsInit(ConnectionHandle hdl);

    // Internal helpers
    void setState(WebSocketState new_state);
    void scheduleReconnect();
    void doReconnect();
    void startPingLoop();
    void stopPingLoop();
    void processMessageQueue();
    bool sendImpl(const std::string& message, websocketpp::frame::opcode::value opcode);
    void runIOThread();
    void stopIOThread();
    uint32_t calculateBackoff() const;

    // Configuration
    WebSocketConfig config_;

    // WebSocket client and connection
    std::unique_ptr<Client> client_;
    ConnectionHandle connection_;
    std::weak_ptr<void> connection_weak_;

    // Threading
    std::unique_ptr<std::thread> io_thread_;
    std::unique_ptr<std::thread> ping_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ping_running_{false};

    // State management
    std::atomic<WebSocketState> state_{WebSocketState::Disconnected};
    std::atomic<uint32_t> reconnect_attempts_{0};

    // Message queue
    std::queue<std::pair<std::string, websocketpp::frame::opcode::value>> message_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Ping/pong tracking
    std::atomic<bool> pong_received_{true};
    std::chrono::steady_clock::time_point last_pong_time_;
    mutable std::mutex pong_mutex_;

    // Statistics
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> messages_received_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> bytes_received_{0};
    std::chrono::steady_clock::time_point last_message_time_;
    mutable std::mutex stats_mutex_;

    // Callbacks
    OnOpenCallback on_open_;
    OnCloseCallback on_close_;
    OnMessageCallback on_message_;
    OnErrorCallback on_error_;
    OnReconnectCallback on_reconnect_;
    OnStateChangeCallback on_state_change_;

    // Reconnection timer
    std::unique_ptr<boost::asio::steady_timer> reconnect_timer_;
    mutable std::mutex reconnect_mutex_;
};

// Factory function
std::shared_ptr<WebSocketClient> createWebSocketClient(const WebSocketConfig& config);

} // namespace network
} // namespace hft
