#pragma once

#include "common/types.hpp"
#include <string>
#include <vector>
#include <stdexcept>

namespace arb {

struct ExchangeConfig {
    std::string exchange_type = "hyperliquid";  // "hyperliquid" or "binance"

    // Binance-specific
    std::string api_key_env = "BINANCE_API_KEY";
    std::string api_secret_env = "BINANCE_API_SECRET";
    std::string spot_rest_url = "https://testnet.binance.vision";
    std::string spot_ws_url = "wss://testnet.binance.vision/ws";
    std::string futures_rest_url = "https://testnet.binancefuture.com";
    std::string futures_ws_url = "wss://stream.binancefuture.com/ws";

    // Hyperliquid-specific
    std::string wallet_private_key_env = "HL_PRIVATE_KEY";
    std::string hl_rest_url = "https://api.hyperliquid-testnet.xyz";
    std::string hl_user_address = "";  // 0x... (auto-derived if empty)
    bool hl_use_api_wallet = false;

    bool testnet = true;
    int reconnect_delay_ms = 1000;
    int max_reconnect_attempts = 10;
    int connection_timeout_ms = 10000;
};

struct FeeConfig {
    double perp_taker_fee_pct = 0.00045;   // 0.045%
    double perp_maker_fee_pct = 0.00015;   // 0.015%
    double spot_taker_fee_pct = 0.00070;   // 0.070%
    double spot_maker_fee_pct = 0.00040;   // 0.040%
};

struct StrategyConfig {
    std::vector<std::string> symbols = {"ETHUSDT"};

    // Hedge mode
    HedgeMode hedge_mode = HedgeMode::USDC_COLLATERAL;

    // Funding rate thresholds
    double min_funding_rate = 0.00002;         // Per-period (hourly for HL)
    double min_annual_yield_pct = 10.0;
    double min_avg_funding_rate = 0.00001;

    // Basis thresholds
    double max_basis_deviation_pct = 1.0;

    // Scoring / entry
    int funding_rate_lookback_periods = 72;    // 72 hourly periods = 3 days
    double entry_score_threshold = 0.5;

    // Position management
    int max_positions = 1;
    double rebalance_threshold_pct = 5.0;
    int max_holding_periods = 720;             // 720 hours = 30 days

    // Funding period config
    int funding_periods_per_day = 24;          // 24 for Hyperliquid, 3 for Binance

    // Fee-aware sizing
    double min_notional_usd = 10.0;
    double max_fee_breakeven_days = 14.0;

    // HLP vault allocation (0-100% of capital to put in HLP)
    double hlp_allocation_pct = 0.0;
};

struct RiskConfig {
    double max_total_exposure_usd = 50.0;
    double max_per_symbol_exposure_usd = 25.0;
    double max_drawdown_pct = 10.0;
    double max_daily_loss_usd = 5.0;
    int futures_leverage = 1;
    std::string margin_type = "CROSS";
    double stop_loss_basis_bps = 200.0;
    double max_funding_rate_to_pay = -0.00002;
    int max_orders_per_minute = 5;
};

struct CapitalConfig {
    double starting_balance_usd = 50.0;
    double allocation_pct = 50.0;
};

struct LoggingConfig {
    std::string log_dir = "./logs";
    std::string log_level = "info";
    bool log_to_console = true;
    bool log_to_file = true;
};

struct PollingConfig {
    int funding_rate_interval_s = 30;
    int price_update_interval_ms = 2000;
    int strategy_eval_interval_s = 60;
    int hedge_check_interval_s = 30;
    int pnl_snapshot_interval_s = 300;
};

struct Config {
    TradingMode mode = TradingMode::PAPER;

    ExchangeConfig exchange;
    FeeConfig fees;
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
