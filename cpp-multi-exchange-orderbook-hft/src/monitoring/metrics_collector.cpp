#include "monitoring/metrics_collector.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>

namespace hft {
namespace monitoring {

// ============================================================================
// Simple HTTP Server Implementation
// ============================================================================

void MetricsCollector::httpServerLoop() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return;
    }

    // Set socket options
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Set non-blocking with timeout
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(http_port_);

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        close(server_fd);
        return;
    }

    if (listen(server_fd, 10) < 0) {
        close(server_fd);
        return;
    }

    while (http_running_.load(std::memory_order_acquire)) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               reinterpret_cast<struct sockaddr*>(&client_addr),
                               &client_len);

        if (client_fd < 0) {
            // Timeout or error, check if we should continue
            continue;
        }

        // Read request (we only care about GET /metrics)
        char buffer[1024];
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';

            // Check if it's a GET request for /metrics
            bool is_metrics_request = (strstr(buffer, "GET /metrics") != nullptr ||
                                       strstr(buffer, "GET / ") != nullptr);

            std::string response;
            if (is_metrics_request) {
                std::string metrics = getMetricsText();

                std::ostringstream oss;
                oss << "HTTP/1.1 200 OK\r\n";
                oss << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n";
                oss << "Content-Length: " << metrics.size() << "\r\n";
                oss << "Connection: close\r\n";
                oss << "\r\n";
                oss << metrics;
                response = oss.str();
            } else {
                // Health check or unknown endpoint
                std::string body = "OK\n";
                std::ostringstream oss;
                oss << "HTTP/1.1 200 OK\r\n";
                oss << "Content-Type: text/plain\r\n";
                oss << "Content-Length: " << body.size() << "\r\n";
                oss << "Connection: close\r\n";
                oss << "\r\n";
                oss << body;
                response = oss.str();
            }

            // Send response
            write(client_fd, response.c_str(), response.size());
        }

        close(client_fd);
    }

    close(server_fd);
}

} // namespace monitoring
} // namespace hft
