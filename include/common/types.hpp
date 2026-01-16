#pragma once

#include <string>
#include <chrono>
#include <optional>
#include <variant>
#include <cstdint>

namespace arb {

// Time types
using Timestamp = std::chrono::time_point<std::chrono::steady_clock>;
using WallClock = std::chrono::time_point<std::chrono::system_clock>;
using Duration = std::chrono::nanoseconds;

inline Timestamp now() {
    return std::chrono::steady_clock::now();
}

inline WallClock wall_now() {
    return std::chrono::system_clock::now();
}

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Price type (in cents for precision, 1.0 = 100 cents = $1.00)
using Price = double;
using Size = double;
using Notional = double;

// Side enum
enum class Side {
    BUY,
    SELL
};

inline std::string side_to_string(Side s) {
    return s == Side::BUY ? "BUY" : "SELL";
}

// Order types
enum class OrderType {
    LIMIT,
    MARKET,
    IOC,  // Immediate or Cancel
    FOK,  // Fill or Kill
    GTC   // Good Till Cancel
};

inline std::string order_type_to_string(OrderType t) {
    switch (t) {
        case OrderType::LIMIT: return "LIMIT";
        case OrderType::MARKET: return "MARKET";
        case OrderType::IOC: return "IOC";
        case OrderType::FOK: return "FOK";
        case OrderType::GTC: return "GTC";
    }
    return "UNKNOWN";
}

// Order state
enum class OrderState {
    PENDING,      // Created but not sent
    SENT,         // Sent to exchange
    ACKNOWLEDGED, // Exchange confirmed receipt
    PARTIAL,      // Partially filled
    FILLED,       // Fully filled
    CANCELED,     // Canceled by user
    REJECTED,     // Rejected by exchange
    EXPIRED       // TTL expired
};

inline std::string order_state_to_string(OrderState s) {
    switch (s) {
        case OrderState::PENDING: return "PENDING";
        case OrderState::SENT: return "SENT";
        case OrderState::ACKNOWLEDGED: return "ACKNOWLEDGED";
        case OrderState::PARTIAL: return "PARTIAL";
        case OrderState::FILLED: return "FILLED";
        case OrderState::CANCELED: return "CANCELED";
        case OrderState::REJECTED: return "REJECTED";
        case OrderState::EXPIRED: return "EXPIRED";
    }
    return "UNKNOWN";
}

// Trading mode
enum class TradingMode {
    DRY_RUN,  // Compute signals only, no orders
    PAPER,    // Simulated execution
    LIVE      // Real orders
};

inline std::string mode_to_string(TradingMode m) {
    switch (m) {
        case TradingMode::DRY_RUN: return "DRY_RUN";
        case TradingMode::PAPER: return "PAPER";
        case TradingMode::LIVE: return "LIVE";
    }
    return "UNKNOWN";
}

// Connection status
enum class ConnectionStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    RECONNECTING,
    ERROR
};

inline std::string conn_status_to_string(ConnectionStatus s) {
    switch (s) {
        case ConnectionStatus::DISCONNECTED: return "DISCONNECTED";
        case ConnectionStatus::CONNECTING: return "CONNECTING";
        case ConnectionStatus::CONNECTED: return "CONNECTED";
        case ConnectionStatus::RECONNECTING: return "RECONNECTING";
        case ConnectionStatus::ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

// Price level in order book
struct PriceLevel {
    Price price{0.0};
    Size size{0.0};

    bool operator==(const PriceLevel& other) const {
        return price == other.price && size == other.size;
    }
};

// Market outcome (for binary markets)
struct Outcome {
    std::string token_id;
    std::string name;  // e.g., "YES" or "NO"
    Price best_bid{0.0};
    Size bid_size{0.0};
    Price best_ask{0.0};
    Size ask_size{0.0};
    Price last_trade_price{0.0};
    Timestamp last_update;
};

// Market info
struct Market {
    std::string market_id;
    std::string condition_id;
    std::string question;
    std::string slug;
    Outcome yes_outcome;
    Outcome no_outcome;
    bool active{true};
    WallClock end_date;
    double fee_rate_bps{0.0};  // Fee rate in basis points
};

// BTC reference price
struct BtcPrice {
    Price bid{0.0};
    Price ask{0.0};
    Price mid{0.0};
    Price last{0.0};
    Timestamp timestamp;
    int64_t exchange_time_ms{0};
};

// Trade execution record
struct Fill {
    std::string order_id;
    std::string trade_id;
    std::string market_id;
    std::string token_id;
    Side side;
    Price price;
    Size size;
    Notional notional;
    Notional fee;
    Timestamp fill_time;
    int64_t exchange_time_ms{0};
};

// Signal from strategy
struct Signal {
    std::string strategy_name;
    std::string market_id;
    std::string token_id;
    Side side;
    Price target_price;
    Size target_size;
    double expected_edge;  // Expected profit in cents
    double confidence;     // 0.0 to 1.0
    Timestamp generated_at;
    std::string reason;
};

// Latency metrics
struct LatencyMetrics {
    Duration market_update_to_decision;
    Duration decision_to_order_send;
    Duration order_send_to_ack;
    Duration ack_to_fill;
    Duration total_round_trip;

    // Aggregates
    Duration p50_decision_to_send;
    Duration p95_decision_to_send;
    int64_t samples{0};
};

} // namespace arb
