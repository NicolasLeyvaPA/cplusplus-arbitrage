#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>
#include "common/types.hpp"

namespace arb {

struct RiskConfig {
    double max_notional_per_trade{1.50};     // Max $1.50 per trade with $50 bankroll
    double max_daily_loss{5.0};              // Max $5 daily loss (10% of bankroll)
    double max_open_positions{3};            // Max 3 open positions
    double max_exposure_per_market{3.0};     // Max $3 in single market
    double stop_loss_threshold{0.10};        // Stop trading if down 10%
    double slippage_threshold_bps{50.0};     // Alert if slippage > 50bps
    int max_orders_per_minute{10};           // Rate limit
};

struct StrategyConfig {
    // Underpricing (S2) strategy
    double min_edge_cents{2.0};              // Minimum 2 cents edge after fees
    double max_spread_to_trade{0.05};        // Don't trade if spread > 5%

    // Stale-odds (S1) strategy
    double lag_move_threshold_bps{25.0};     // BTC move > 25bps triggers signal
    int staleness_window_ms{500};            // Consider stale if no update in 500ms
    double min_confidence{0.6};              // Minimum confidence to trade

    // Common
    double target_fill_rate{0.95};           // Target 95% fill rate
    bool enable_s1{true};
    bool enable_s2{true};
    bool enable_s3{false};                   // Market making disabled by default
};

struct ConnectionConfig {
    // Polymarket
    std::string polymarket_rest_url{"https://clob.polymarket.com"};
    std::string polymarket_ws_url{"wss://ws-subscriptions-clob.polymarket.com/ws/market"};
    std::string polymarket_gamma_url{"https://gamma-api.polymarket.com"};

    // Binance
    std::string binance_ws_url{"wss://stream.binance.com:9443/ws"};
    std::string binance_symbol{"btcusdt"};

    // Connection params
    int reconnect_delay_ms{1000};
    int max_reconnect_attempts{10};
    int heartbeat_interval_ms{30000};
    int connection_timeout_ms{10000};
};

struct LoggingConfig {
    std::string log_dir{"./logs"};
    std::string log_level{"info"};           // debug, info, warn, error
    bool log_to_console{true};
    bool log_to_file{true};
    bool json_format{true};                  // JSON lines format
    int max_log_file_size_mb{100};
    int max_log_files{5};
};

struct Config {
    TradingMode mode{TradingMode::DRY_RUN};
    double starting_balance_usdc{50.0};

    RiskConfig risk;
    StrategyConfig strategy;
    ConnectionConfig connection;
    LoggingConfig logging;

    std::string trade_ledger_path{"./data/trades.json"};
    std::string state_snapshot_path{"./data/state.json"};

    // Market selection
    std::vector<std::string> market_slugs;   // Specific markets to trade
    std::string market_pattern{"btc.*15m"};  // Regex pattern for market selection

    // Load from file
    static Config load(const std::string& path);

    // Save to file
    void save(const std::string& path) const;

    // Validate configuration
    bool validate() const;

    // Get environment variable with default
    static std::string get_env(const std::string& name, const std::string& default_val = "");
};

// JSON serialization
void to_json(nlohmann::json& j, const Config& c);
void from_json(const nlohmann::json& j, Config& c);

} // namespace arb
