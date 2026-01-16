#pragma once

#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include "common/types.hpp"

namespace arb {

/**
 * Base WebSocket client with reconnection logic.
 * Uses a simple polling-based approach for portability.
 */
class WebSocketClientBase {
public:
    using MessageCallback = std::function<void(const std::string&, Timestamp)>;
    using StatusCallback = std::function<void(ConnectionStatus)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    WebSocketClientBase(const std::string& url, const std::string& name);
    virtual ~WebSocketClientBase();

    // Connection management
    virtual void connect();
    virtual void disconnect();
    virtual void reconnect();

    // Send message
    virtual bool send(const std::string& message);

    // Status
    ConnectionStatus status() const { return status_.load(); }
    bool is_connected() const { return status_.load() == ConnectionStatus::CONNECTED; }

    // Callbacks
    void set_message_callback(MessageCallback cb) { on_message_ = std::move(cb); }
    void set_status_callback(StatusCallback cb) { on_status_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { on_error_ = std::move(cb); }

    // Configuration
    void set_reconnect_delay(int ms) { reconnect_delay_ms_ = ms; }
    void set_max_reconnect_attempts(int n) { max_reconnect_attempts_ = n; }

    // Stats
    int64_t messages_received() const { return messages_received_.load(); }
    int64_t bytes_received() const { return bytes_received_.load(); }
    Timestamp last_message_time() const;

protected:
    std::string url_;
    std::string name_;
    std::atomic<ConnectionStatus> status_{ConnectionStatus::DISCONNECTED};

    MessageCallback on_message_;
    StatusCallback on_status_;
    ErrorCallback on_error_;

    int reconnect_delay_ms_{1000};
    int max_reconnect_attempts_{10};
    int reconnect_attempts_{0};

    std::atomic<int64_t> messages_received_{0};
    std::atomic<int64_t> bytes_received_{0};
    Timestamp last_message_time_;
    mutable std::mutex time_mutex_;

    std::atomic<bool> running_{false};
    std::thread recv_thread_;

    // Platform-specific socket handle
    void* socket_handle_{nullptr};

    virtual void run_receive_loop();
    virtual void handle_message(const std::string& msg);
    void set_status(ConnectionStatus s);

    // SSL context for secure connections
    void* ssl_ctx_{nullptr};
};

} // namespace arb
