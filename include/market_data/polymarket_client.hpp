#pragma once

#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/types.hpp"
#include "config/config.hpp"
#include "market_data/order_book.hpp"

namespace arb {

/**
 * Polymarket CLOB client for market data and order management.
 * Connects to both REST API for market discovery and WebSocket for real-time updates.
 */
class PolymarketClient {
public:
    using BookCallback = std::function<void(const std::string& market_id, const std::string& token_id)>;
    using TradeCallback = std::function<void(const Fill&)>;
    using StatusCallback = std::function<void(ConnectionStatus)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit PolymarketClient(const ConnectionConfig& config);
    ~PolymarketClient();

    // Market discovery (REST)
    std::vector<Market> fetch_markets();
    std::vector<Market> fetch_btc_markets();  // Filtered to BTC up/down markets
    std::optional<Market> fetch_market(const std::string& condition_id);

    // Order book (REST)
    void fetch_order_book(const std::string& token_id, OrderBook& book);

    // WebSocket connection
    void connect();
    void disconnect();

    // Subscribe to market updates
    void subscribe_market(const std::string& token_id);
    void unsubscribe_market(const std::string& token_id);

    // Callbacks
    void set_book_callback(BookCallback cb) { on_book_update_ = std::move(cb); }
    void set_trade_callback(TradeCallback cb) { on_trade_ = std::move(cb); }
    void set_status_callback(StatusCallback cb) { on_status_ = std::move(cb); }
    void set_error_callback(ErrorCallback cb) { on_error_ = std::move(cb); }

    // Order management (for paper/live trading)
    struct OrderRequest {
        std::string token_id;
        Side side;
        Price price;
        Size size;
        OrderType type{OrderType::GTC};
    };

    struct OrderResponse {
        bool success{false};
        std::string order_id;
        std::string error_message;
        int64_t exchange_time_ms{0};
    };

    // These require authentication
    OrderResponse place_order(const OrderRequest& req);
    OrderResponse submit_order(const Order& order);  // Alternative submission method

    struct CancelResponse {
        bool success{false};
        std::string error;
    };
    CancelResponse cancel_order(const std::string& order_id);

    std::vector<Fill> get_trades(const std::string& market_id);

    // Order query (for reconciliation)
    std::optional<Order> get_order(const std::string& order_id);

    struct OpenOrdersResponse {
        bool success{false};
        std::vector<Order> orders;
        std::string error;
    };
    OpenOrdersResponse get_open_orders();

    // Position query (for reconciliation)
    struct PositionInfo {
        std::string market_id;
        std::string token_id;
        std::string outcome;
        double size{0.0};
        double avg_price{0.0};
        double unrealized_pnl{0.0};
        double realized_pnl{0.0};
    };
    struct PositionsResponse {
        bool success{false};
        std::vector<PositionInfo> positions;
        std::string error;
    };
    PositionsResponse get_positions();

    // Balance query (for reconciliation)
    struct BalanceResponse {
        bool success{false};
        double available{0.0};
        double locked{0.0};
        std::string error;
    };
    BalanceResponse get_balance();

    // Get book reference (for direct access)
    BinaryMarketBook* get_market_book(const std::string& market_id);

    // Status
    ConnectionStatus status() const { return status_.load(); }
    bool is_connected() const { return status_.load() == ConnectionStatus::CONNECTED; }

    // Stats
    int64_t messages_received() const { return messages_received_.load(); }
    Timestamp last_update_time() const;

    // API credentials (from environment)
    void set_api_credentials(const std::string& key, const std::string& secret, const std::string& passphrase);
    bool has_credentials() const { return !api_key_.empty(); }

private:
    ConnectionConfig config_;

    BookCallback on_book_update_;
    TradeCallback on_trade_;
    StatusCallback on_status_;
    ErrorCallback on_error_;

    std::atomic<ConnectionStatus> status_{ConnectionStatus::DISCONNECTED};
    std::atomic<bool> running_{false};
    std::thread recv_thread_;

    // Market books keyed by market_id
    std::map<std::string, std::unique_ptr<BinaryMarketBook>> market_books_;
    std::mutex books_mutex_;

    // Token ID to market ID mapping
    std::map<std::string, std::string> token_to_market_;

    // API credentials
    std::string api_key_;
    std::string api_secret_;
    std::string api_passphrase_;

    // Stats
    std::atomic<int64_t> messages_received_{0};
    Timestamp last_update_time_;
    mutable std::mutex time_mutex_;

    // Socket handles
    void* socket_{nullptr};
    void* ssl_ctx_{nullptr};
    void* ssl_{nullptr};

    void run_connection_loop();
    void parse_message(const std::string& msg, Timestamp recv_time);
    void parse_book_message(const nlohmann::json& data, Timestamp recv_time);
    void parse_price_change(const nlohmann::json& data, Timestamp recv_time);
    void parse_trade_message(const nlohmann::json& data, Timestamp recv_time);

    // HTTP helpers for REST API
    std::string http_get(const std::string& url);
    std::string http_post(const std::string& url, const std::string& body);

    // Low-level socket operations
    bool connect_socket();
    void disconnect_socket();
    bool send_raw(const std::string& data);
    std::string recv_frame();
    void send_pong(const std::string& payload);

    // Authentication header generation
    std::string generate_l2_signature(const std::string& timestamp, const std::string& method,
                                       const std::string& path, const std::string& body);
};

} // namespace arb
