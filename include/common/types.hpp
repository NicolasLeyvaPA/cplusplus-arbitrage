#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cmath>
#include <cstdint>

namespace arb {

// ============================================================================
// TIME TYPES
// ============================================================================

using Timestamp = std::chrono::time_point<std::chrono::steady_clock>;
using WallClock = std::chrono::time_point<std::chrono::system_clock>;
using Duration = std::chrono::nanoseconds;

inline Timestamp now() { return std::chrono::steady_clock::now(); }
inline WallClock wall_now() { return std::chrono::system_clock::now(); }

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// VALUE TYPES
// ============================================================================

using Price = double;
using Size = double;      // Quantity in base asset (e.g., BTC)
using Notional = double;  // Price * Size in quote asset (e.g., USDT)

// ============================================================================
// ENUMS
// ============================================================================

enum class Side { BUY, SELL };
enum class OrderType { MARKET, LIMIT, LIMIT_MAKER };
enum class TimeInForce { GTC, IOC, FOK };
enum class OrderStatus { NEW, PARTIALLY_FILLED, FILLED, CANCELED, REJECTED, EXPIRED };
enum class TradingMode { DRY_RUN, PAPER, LIVE };
enum class MarketType { SPOT, FUTURES };
enum class PositionSide { LONG, SHORT, BOTH };
enum class ConnectionStatus { DISCONNECTED, CONNECTING, CONNECTED, RECONNECTING, ERROR };
enum class HedgeMode { FULL_HEDGE, USDC_COLLATERAL };

// ============================================================================
// MARKET DATA
// ============================================================================

struct Ticker {
    std::string symbol;
    Price bid{0};
    Price ask{0};
    Size bid_qty{0};
    Size ask_qty{0};
    uint64_t timestamp_ms{0};

    Price mid() const { return (bid + ask) / 2.0; }
    Price spread() const { return ask - bid; }
    double spread_bps() const {
        auto m = mid();
        return m > 0 ? (ask - bid) / m * 10000.0 : 0;
    }
};

struct FundingInfo {
    std::string symbol;
    double current_rate{0};      // Per-period rate (e.g., 0.0001 = 0.01%)
    double predicted_rate{0};    // Next predicted rate
    Price mark_price{0};
    Price index_price{0};
    uint64_t next_funding_time_ms{0};
    uint64_t timestamp_ms{0};
    int funding_periods_per_day{3};  // Binance=3 (8h), Hyperliquid=24 (1h)

    double annualized_rate() const {
        return current_rate * static_cast<double>(funding_periods_per_day) * 365.0 * 100.0;
    }
};

struct BasisInfo {
    std::string symbol;
    Price spot_mid{0};
    Price futures_mid{0};
    double basis{0};             // (futures - spot) / spot
    double basis_bps{0};         // basis * 10000
    double annualized_basis{0};
    uint64_t timestamp_ms{0};
};

// ============================================================================
// ACCOUNT
// ============================================================================

struct AccountBalance {
    double total_balance{0};
    double available_balance{0};
    double unrealized_pnl{0};
    double margin_balance{0};
    std::map<std::string, double> asset_balances;
};

// ============================================================================
// DELTA-NEUTRAL POSITION
// ============================================================================

struct DeltaNeutralPosition {
    std::string symbol;

    // Spot leg
    Size spot_size{0};           // Positive = long
    Price spot_avg_entry{0};
    Notional spot_notional{0};

    // Futures leg
    Size futures_size{0};        // Negative = short
    Price futures_avg_entry{0};
    Notional futures_notional{0};

    // P&L tracking
    double funding_collected{0};
    int funding_periods{0};
    double realized_pnl{0};
    double unrealized_pnl{0};
    double total_fees{0};

    // Timestamps
    uint64_t entry_time_ms{0};
    uint64_t last_update_ms{0};

    // Computed properties
    double net_delta() const { return spot_size + futures_size; }

    bool is_balanced(double tolerance_pct = 1.0, HedgeMode mode = HedgeMode::FULL_HEDGE) const {
        if (mode == HedgeMode::USDC_COLLATERAL) return true;  // No spot leg to balance
        if (spot_size == 0) return futures_size == 0;
        return std::abs(net_delta()) < std::abs(spot_size) * (tolerance_pct / 100.0);
    }

    double total_pnl() const {
        return realized_pnl + unrealized_pnl + funding_collected - total_fees;
    }

    Notional total_notional() const {
        return std::abs(spot_notional) + std::abs(futures_notional);
    }

    bool is_open() const { return spot_size != 0 || futures_size != 0; }
};

// ============================================================================
// SIGNALS
// ============================================================================

enum class SignalType { OPEN, CLOSE, REBALANCE };

struct DeltaNeutralSignal {
    SignalType type{SignalType::OPEN};
    std::string symbol;
    Size target_size{0};
    double expected_funding_rate{0};
    double expected_annual_yield{0};
    double confidence{0};
    std::string reason;
};

// ============================================================================
// ORDER REQUEST / RESPONSE
// ============================================================================

struct OrderRequest {
    std::string symbol;
    MarketType market{MarketType::SPOT};
    Side side{Side::BUY};
    OrderType type{OrderType::MARKET};
    TimeInForce tif{TimeInForce::GTC};
    Size quantity{0};
    Price price{0};
    std::string client_order_id;
    bool reduce_only{false};
};

struct OrderResponse {
    std::string order_id;
    std::string client_order_id;
    std::string symbol;
    OrderStatus status{OrderStatus::NEW};
    Side side{Side::BUY};
    Price price{0};
    Size original_qty{0};
    Size executed_qty{0};
    Notional cumulative_quote_qty{0};
    uint64_t timestamp_ms{0};
    std::string error_message;

    bool success() const {
        return error_message.empty() && status != OrderStatus::REJECTED;
    }
};

// ============================================================================
// INLINE STRING CONVERSIONS
// ============================================================================

inline std::string to_string(Side s) {
    return s == Side::BUY ? "BUY" : "SELL";
}

inline std::string to_string(MarketType m) {
    return m == MarketType::SPOT ? "SPOT" : "FUTURES";
}

inline std::string to_string(TradingMode m) {
    switch (m) {
        case TradingMode::DRY_RUN: return "DRY_RUN";
        case TradingMode::PAPER: return "PAPER";
        case TradingMode::LIVE: return "LIVE";
    }
    return "UNKNOWN";
}

inline std::string to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::NEW: return "NEW";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED: return "FILLED";
        case OrderStatus::CANCELED: return "CANCELED";
        case OrderStatus::REJECTED: return "REJECTED";
        case OrderStatus::EXPIRED: return "EXPIRED";
    }
    return "UNKNOWN";
}

inline std::string to_string(SignalType t) {
    switch (t) {
        case SignalType::OPEN: return "OPEN";
        case SignalType::CLOSE: return "CLOSE";
        case SignalType::REBALANCE: return "REBALANCE";
    }
    return "UNKNOWN";
}

inline std::string to_string(ConnectionStatus s) {
    switch (s) {
        case ConnectionStatus::DISCONNECTED: return "DISCONNECTED";
        case ConnectionStatus::CONNECTING: return "CONNECTING";
        case ConnectionStatus::CONNECTED: return "CONNECTED";
        case ConnectionStatus::RECONNECTING: return "RECONNECTING";
        case ConnectionStatus::ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

inline Side side_from_string(const std::string& s) {
    return s == "SELL" ? Side::SELL : Side::BUY;
}

inline OrderStatus order_status_from_str(const std::string& s) {
    if (s == "NEW") return OrderStatus::NEW;
    if (s == "PARTIALLY_FILLED") return OrderStatus::PARTIALLY_FILLED;
    if (s == "FILLED") return OrderStatus::FILLED;
    if (s == "CANCELED") return OrderStatus::CANCELED;
    if (s == "REJECTED") return OrderStatus::REJECTED;
    if (s == "EXPIRED") return OrderStatus::EXPIRED;
    return OrderStatus::NEW;
}

inline TradingMode trading_mode_from_str(const std::string& s) {
    if (s == "LIVE" || s == "live") return TradingMode::LIVE;
    if (s == "PAPER" || s == "paper") return TradingMode::PAPER;
    return TradingMode::DRY_RUN;
}

inline std::string to_string(HedgeMode m) {
    return m == HedgeMode::FULL_HEDGE ? "FULL_HEDGE" : "USDC_COLLATERAL";
}

inline HedgeMode hedge_mode_from_str(const std::string& s) {
    if (s == "FULL_HEDGE" || s == "full_hedge") return HedgeMode::FULL_HEDGE;
    return HedgeMode::USDC_COLLATERAL;
}

} // namespace arb
