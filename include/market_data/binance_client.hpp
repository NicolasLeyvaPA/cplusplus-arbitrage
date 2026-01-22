#pragma once

#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <nlohmann/json.hpp>
#include "common/types.hpp"
#include "config/config.hpp"

namespace arb {

/**
 * Binance WebSocket client for BTC price feed.
 * Subscribes to bookTicker stream for real-time best bid/ask.
 */
class BinanceClient {
public:
    using PriceCallback = std::function<void(const BtcPrice&)>;
    using StatusCallback = std::function<void(ConnectionStatus)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit BinanceClient(const ConnectionConfig& config);
    ~BinanceClient();

    // Connection management
    void connect();
    void disconnect();

    // Callbacks
    void set_price_callback(PriceCallback cb) { on_price_ = std::move(cb); }
    void set_status_callback(StatusCallback cb) { on_status_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { on_error_ = std::move(cb); }

    // Current price snapshot
    BtcPrice current_price() const;

    // Status
    ConnectionStatus status() const { return status_.load(); }
    bool is_connected() const { return status_.load() == ConnectionStatus::CONNECTED; }

    // Stats
    int64_t messages_received() const { return messages_received_.load(); }
    Timestamp last_update_time() const;

private:
    ConnectionConfig config_;
    std::string ws_url_;

    PriceCallback on_price_;
    StatusCallback on_status_;
    ErrorCallback on_error_;

    std::atomic<ConnectionStatus> status_{ConnectionStatus::DISCONNECTED};
    std::atomic<bool> running_{false};
    std::thread recv_thread_;

    BtcPrice current_price_;
    mutable std::mutex price_mutex_;

    std::atomic<int64_t> messages_received_{0};
    Timestamp last_update_time_{};

    // Socket handle (platform-specific)
    void* socket_{nullptr};
    void* ssl_ctx_{nullptr};
    void* ssl_{nullptr};

    void run_connection_loop();
    void parse_book_ticker(const std::string& msg, Timestamp recv_time);
    void parse_trade(const std::string& msg, Timestamp recv_time);

    // Low-level socket operations
    bool connect_socket();
    void disconnect_socket();
    bool send_raw(const std::string& data);
    std::string recv_frame();
    void send_pong(const std::string& payload);
};

} // namespace arb
