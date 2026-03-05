#include "network/websocket_client.hpp"

#include <boost/asio/ssl.hpp>
#include <iostream>
#include <sstream>

namespace hft {
namespace network {

WebSocketClient::WebSocketClient(const WebSocketConfig& config)
    : config_(config) {
    // Initialize websocketpp client
    client_ = std::make_unique<Client>();

    // Set logging level (disable in production for performance)
    client_->clear_access_channels(websocketpp::log::alevel::all);
    client_->clear_error_channels(websocketpp::log::elevel::all);

    // Initialize ASIO
    client_->init_asio();

    // Set TLS handler
    client_->set_tls_init_handler([this](ConnectionHandle hdl) {
        return this->onTlsInit(hdl);
    });

    // Set event handlers
    client_->set_open_handler([this](ConnectionHandle hdl) {
        this->onOpen(hdl);
    });

    client_->set_close_handler([this](ConnectionHandle hdl) {
        this->onClose(hdl);
    });

    client_->set_message_handler([this](ConnectionHandle hdl, Client::message_ptr msg) {
        this->onMessage(hdl, msg);
    });

    client_->set_fail_handler([this](ConnectionHandle hdl) {
        this->onFail(hdl);
    });

    client_->set_ping_handler([this](ConnectionHandle hdl, const std::string& payload) {
        return this->onPing(hdl, payload);
    });

    client_->set_pong_handler([this](ConnectionHandle hdl, const std::string& payload) {
        this->onPong(hdl, payload);
    });

    client_->set_pong_timeout_handler([this](ConnectionHandle hdl, const std::string& payload) {
        this->onPongTimeout(hdl, payload);
    });
}

WebSocketClient::~WebSocketClient() {
    disconnect(websocketpp::close::status::going_away, "Client destroyed");
    stopPingLoop();
    stopIOThread();
}

bool WebSocketClient::connect() {
    if (state_.load() == WebSocketState::Connected ||
        state_.load() == WebSocketState::Connecting) {
        return false;
    }

    setState(WebSocketState::Connecting);

    websocketpp::lib::error_code ec;
    Client::connection_ptr con = client_->get_connection(config_.url, ec);

    if (ec) {
        setState(WebSocketState::Failed);
        if (on_error_) {
            on_error_("Connection error: " + ec.message());
        }
        return false;
    }

    // Add custom headers
    for (const auto& [key, value] : config_.headers) {
        con->append_header(key, value);
    }

    // Store connection handle
    connection_ = con->get_handle();

    // Connect
    client_->connect(con);

    // Start IO thread if not running
    if (!running_.load()) {
        running_ = true;
        io_thread_ = std::make_unique<std::thread>([this]() {
            this->runIOThread();
        });
    }

    return true;
}

void WebSocketClient::disconnect(int code, const std::string& reason) {
    if (state_.load() == WebSocketState::Disconnected ||
        state_.load() == WebSocketState::Closing) {
        return;
    }

    setState(WebSocketState::Closing);
    config_.auto_reconnect = false;  // Prevent auto-reconnect on manual disconnect

    // Cancel reconnect timer
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        if (reconnect_timer_) {
            reconnect_timer_->cancel();
        }
    }

    // Close connection
    websocketpp::lib::error_code ec;
    client_->close(connection_, static_cast<websocketpp::close::status::value>(code), reason, ec);

    if (ec) {
        // Force stop if close fails
        client_->stop();
    }
}

void WebSocketClient::reconnect() {
    if (state_.load() == WebSocketState::Connected) {
        disconnect(websocketpp::close::status::normal, "Reconnecting");
    }

    reconnect_attempts_ = 0;
    config_.auto_reconnect = true;
    scheduleReconnect();
}

bool WebSocketClient::send(const std::string& message, MessageType type) {
    websocketpp::frame::opcode::value opcode;
    switch (type) {
        case MessageType::Text:
            opcode = websocketpp::frame::opcode::text;
            break;
        case MessageType::Binary:
            opcode = websocketpp::frame::opcode::binary;
            break;
        case MessageType::Ping:
            opcode = websocketpp::frame::opcode::ping;
            break;
        case MessageType::Pong:
            opcode = websocketpp::frame::opcode::pong;
            break;
        default:
            return false;
    }

    if (!isConnected()) {
        // Queue message if not connected
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (message_queue_.size() < config_.max_message_queue_size) {
            message_queue_.emplace(message, opcode);
            return true;
        }
        return false;
    }

    return sendImpl(message, opcode);
}

bool WebSocketClient::send(const nlohmann::json& json_message) {
    return send(json_message.dump(), MessageType::Text);
}

bool WebSocketClient::sendBinary(const std::vector<uint8_t>& data) {
    std::string message(data.begin(), data.end());
    return send(message, MessageType::Binary);
}

bool WebSocketClient::sendImpl(const std::string& message, websocketpp::frame::opcode::value opcode) {
    websocketpp::lib::error_code ec;
    client_->send(connection_, message, opcode, ec);

    if (ec) {
        if (on_error_) {
            on_error_("Send error: " + ec.message());
        }
        return false;
    }

    // Update statistics
    messages_sent_++;
    bytes_sent_ += message.size();

    return true;
}

size_t WebSocketClient::queueSize() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return message_queue_.size();
}

void WebSocketClient::clearQueue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::queue<std::pair<std::string, websocketpp::frame::opcode::value>> empty;
    std::swap(message_queue_, empty);
}

std::chrono::steady_clock::time_point WebSocketClient::lastMessageTime() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return last_message_time_;
}

void WebSocketClient::updateConfig(const WebSocketConfig& config) {
    config_ = config;
}

void WebSocketClient::onOpen(ConnectionHandle hdl) {
    setState(WebSocketState::Connected);
    reconnect_attempts_ = 0;
    pong_received_ = true;

    // Process queued messages
    processMessageQueue();

    // Start ping loop
    startPingLoop();

    if (on_open_) {
        on_open_();
    }
}

void WebSocketClient::onClose(ConnectionHandle hdl) {
    auto con = client_->get_con_from_hdl(hdl);
    int code = con->get_remote_close_code();
    std::string reason = con->get_remote_close_reason();

    stopPingLoop();
    setState(WebSocketState::Disconnected);

    if (on_close_) {
        on_close_(code, reason);
    }

    // Auto-reconnect if enabled
    if (config_.auto_reconnect) {
        scheduleReconnect();
    }
}

void WebSocketClient::onMessage(ConnectionHandle hdl, Client::message_ptr msg) {
    // Update statistics
    messages_received_++;
    bytes_received_ += msg->get_payload().size();

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        last_message_time_ = std::chrono::steady_clock::now();
    }

    if (on_message_) {
        MessageType type = (msg->get_opcode() == websocketpp::frame::opcode::binary)
                          ? MessageType::Binary : MessageType::Text;
        on_message_(msg->get_payload(), type);
    }
}

void WebSocketClient::onFail(ConnectionHandle hdl) {
    auto con = client_->get_con_from_hdl(hdl);
    std::string error = con->get_ec().message();

    setState(WebSocketState::Failed);

    if (on_error_) {
        on_error_("Connection failed: " + error);
    }

    // Auto-reconnect if enabled
    if (config_.auto_reconnect) {
        scheduleReconnect();
    }
}

bool WebSocketClient::onPing(ConnectionHandle hdl, const std::string& payload) {
    // Automatically respond with pong (return true)
    return true;
}

void WebSocketClient::onPong(ConnectionHandle hdl, const std::string& payload) {
    pong_received_ = true;

    std::lock_guard<std::mutex> lock(pong_mutex_);
    last_pong_time_ = std::chrono::steady_clock::now();
}

void WebSocketClient::onPongTimeout(ConnectionHandle hdl, const std::string& payload) {
    if (on_error_) {
        on_error_("Pong timeout - connection may be stale");
    }

    // Reconnect on pong timeout
    if (config_.auto_reconnect) {
        disconnect(websocketpp::close::status::going_away, "Pong timeout");
    }
}

WebSocketClient::ContextPtr WebSocketClient::onTlsInit(ConnectionHandle hdl) {
    auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);

    try {
        ctx->set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::single_dh_use
        );

        // Use system default CA certificates
        ctx->set_default_verify_paths();
        ctx->set_verify_mode(boost::asio::ssl::verify_peer);

    } catch (const std::exception& e) {
        if (on_error_) {
            on_error_("TLS initialization error: " + std::string(e.what()));
        }
    }

    return ctx;
}

void WebSocketClient::setState(WebSocketState new_state) {
    WebSocketState old_state = state_.exchange(new_state);

    if (old_state != new_state && on_state_change_) {
        on_state_change_(new_state);
    }
}

void WebSocketClient::scheduleReconnect() {
    if (config_.max_reconnect_attempts > 0 &&
        reconnect_attempts_.load() >= config_.max_reconnect_attempts) {
        setState(WebSocketState::Failed);
        if (on_error_) {
            on_error_("Max reconnect attempts reached");
        }
        return;
    }

    setState(WebSocketState::Reconnecting);
    uint32_t delay = calculateBackoff();
    reconnect_attempts_++;

    if (on_reconnect_) {
        on_reconnect_(reconnect_attempts_.load());
    }

    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    reconnect_timer_ = std::make_unique<boost::asio::steady_timer>(
        client_->get_io_service(),
        std::chrono::milliseconds(delay)
    );

    reconnect_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (!ec) {
            this->doReconnect();
        }
    });
}

void WebSocketClient::doReconnect() {
    if (state_.load() != WebSocketState::Reconnecting) {
        return;
    }

    // Reset client for new connection
    client_->reset();
    connect();
}

uint32_t WebSocketClient::calculateBackoff() const {
    uint32_t attempts = reconnect_attempts_.load();
    double delay = config_.reconnect_initial_delay_ms *
                   std::pow(config_.reconnect_backoff_multiplier, attempts);
    return static_cast<uint32_t>(
        std::min(delay, static_cast<double>(config_.reconnect_max_delay_ms))
    );
}

void WebSocketClient::startPingLoop() {
    if (ping_running_.load() || config_.ping_interval_ms == 0) {
        return;
    }

    ping_running_ = true;
    ping_thread_ = std::make_unique<std::thread>([this]() {
        while (ping_running_.load() && isConnected()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.ping_interval_ms)
            );

            if (!ping_running_.load() || !isConnected()) {
                break;
            }

            // Check if we received pong for last ping
            if (!pong_received_.load()) {
                if (on_error_) {
                    on_error_("No pong received for previous ping");
                }
                // Consider reconnecting
                continue;
            }

            // Send ping
            pong_received_ = false;
            websocketpp::lib::error_code ec;
            client_->ping(connection_, "ping", ec);

            if (ec && on_error_) {
                on_error_("Ping error: " + ec.message());
            }
        }
    });
}

void WebSocketClient::stopPingLoop() {
    ping_running_ = false;
    if (ping_thread_ && ping_thread_->joinable()) {
        ping_thread_->join();
    }
    ping_thread_.reset();
}

void WebSocketClient::processMessageQueue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    while (!message_queue_.empty() && isConnected()) {
        auto& [message, opcode] = message_queue_.front();
        if (sendImpl(message, opcode)) {
            message_queue_.pop();
        } else {
            break;  // Stop on first failure
        }
    }
}

void WebSocketClient::runIOThread() {
    while (running_.load()) {
        try {
            client_->run();
        } catch (const std::exception& e) {
            if (on_error_) {
                on_error_("IO thread exception: " + std::string(e.what()));
            }
        }

        // Brief sleep before restarting run() if still running
        if (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            client_->reset();
        }
    }
}

void WebSocketClient::stopIOThread() {
    running_ = false;

    try {
        client_->stop();
    } catch (...) {
        // Ignore errors during shutdown
    }

    if (io_thread_ && io_thread_->joinable()) {
        io_thread_->join();
    }
    io_thread_.reset();
}

std::shared_ptr<WebSocketClient> createWebSocketClient(const WebSocketConfig& config) {
    return std::make_shared<WebSocketClient>(config);
}

} // namespace network
} // namespace hft
