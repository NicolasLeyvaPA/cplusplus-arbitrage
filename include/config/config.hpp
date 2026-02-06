#pragma once

#include "common/types.hpp"
#include <string>
#include <vector>
#include <stdexcept>

namespace arb {

struct ExchangeConfig {
    std::string api_key_env = "BINANCE_API_KEY";
    std::string api_secret_env = "BINANCE_API_SECRET";
    bool testnet = true;
    std::string spot_rest_url = "https://testnet.binance.vision";
    std::string spot_ws_url = "wss://testnet.binance.vision/ws";
    std::string futures_rest_url = "https://testnet.binancefuture.com";
    std::string futures_ws_url = "wss://stream.binancefuture.com/ws";
    int reconnect_delay_ms = 1000;
    int max_reconnect_attempts = 10;
    int connection_timeout_ms = 10000;
};

struct StrategyConfig {
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT"};

    // Funding rate thresholds
    double min_funding_rate = 0.0001;          // 0.01% per 8h (~13% APY)
    double min_annual_yield_pct = 10.0;        // Minimum annualized yield
    double min_avg_funding_rate = 0.00005;     // Minimum average over lookback

    // Basis thresholds
    double max_basis_deviation_pct = 0.5;      // Max acceptable spot-perp basis

    // Scoring / entry
    int funding_rate_lookback_periods = 24;    // 24 periods = 3 days of history
    double entry_score_threshold = 0.7;        // Min composite score to enter

    // Position management
    int max_positions = 3;                     // Max concurrent delta-neutral pairs
    double rebalance_threshold_pct = 2.0;      // Rebalance hedge at 2% drift
    int max_holding_periods = 90;              // ~30 days max hold
};

struct RiskConfig {
    double max_total_exposure_usd = 10000.0;
    double max_per_symbol_exposure_usd = 5000.0;
    double max_drawdown_pct = 5.0;
    double max_daily_loss_usd = 200.0;
    int futures_leverage = 1;
    std::string margin_type = "ISOLATED";
    double stop_loss_basis_bps = 100.0;        // Close if basis moves 100bps against
    double max_funding_rate_to_pay = -0.0001;  // Close if we'd pay > 0.01%
    int max_orders_per_minute = 10;
};

struct CapitalConfig {
    double starting_balance_usd = 1000.0;
    double allocation_pct = 80.0;              // Deploy 80% of capital
};

struct LoggingConfig {
    std::string log_dir = "./logs";
    std::string log_level = "info";
    bool log_to_console = true;
    bool log_to_file = true;
};

struct PollingConfig {
    int funding_rate_interval_s = 60;          // Poll funding rates every 60s
    int price_update_interval_ms = 1000;       // Update prices every 1s
    int strategy_eval_interval_s = 30;         // Re-evaluate strategy every 30s
    int hedge_check_interval_s = 10;           // Check hedge balance every 10s
    int pnl_snapshot_interval_s = 300;         // Snapshot P&L every 5 minutes
};

struct Config {
    TradingMode mode = TradingMode::PAPER;

    ExchangeConfig exchange;
    StrategyConfig strategy;
    RiskConfig risk;
    CapitalConfig capital;
    LoggingConfig logging;
    PollingConfig polling;

    std::string database_path = "./data/delta_neutral.db";
    std::string trade_ledger_path = "./data/trades.jsonl";

    static Config load(const std::string& path);
    void save(const std::string& path) const;
    void validate() const;

    static std::string get_env(const std::string& name, const std::string& default_val = "");
};

} // namespace arb
