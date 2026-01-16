#include "config/config.hpp"
#include <fstream>
#include <cstdlib>
#include <spdlog/spdlog.h>

namespace arb {

void to_json(nlohmann::json& j, const RiskConfig& c) {
    j = nlohmann::json{
        {"max_notional_per_trade", c.max_notional_per_trade},
        {"max_daily_loss", c.max_daily_loss},
        {"max_open_positions", c.max_open_positions},
        {"max_exposure_per_market", c.max_exposure_per_market},
        {"stop_loss_threshold", c.stop_loss_threshold},
        {"slippage_threshold_bps", c.slippage_threshold_bps},
        {"max_orders_per_minute", c.max_orders_per_minute}
    };
}

void from_json(const nlohmann::json& j, RiskConfig& c) {
    if (j.contains("max_notional_per_trade")) j.at("max_notional_per_trade").get_to(c.max_notional_per_trade);
    if (j.contains("max_daily_loss")) j.at("max_daily_loss").get_to(c.max_daily_loss);
    if (j.contains("max_open_positions")) j.at("max_open_positions").get_to(c.max_open_positions);
    if (j.contains("max_exposure_per_market")) j.at("max_exposure_per_market").get_to(c.max_exposure_per_market);
    if (j.contains("stop_loss_threshold")) j.at("stop_loss_threshold").get_to(c.stop_loss_threshold);
    if (j.contains("slippage_threshold_bps")) j.at("slippage_threshold_bps").get_to(c.slippage_threshold_bps);
    if (j.contains("max_orders_per_minute")) j.at("max_orders_per_minute").get_to(c.max_orders_per_minute);
}

void to_json(nlohmann::json& j, const StrategyConfig& c) {
    j = nlohmann::json{
        {"min_edge_cents", c.min_edge_cents},
        {"max_spread_to_trade", c.max_spread_to_trade},
        {"lag_move_threshold_bps", c.lag_move_threshold_bps},
        {"staleness_window_ms", c.staleness_window_ms},
        {"min_confidence", c.min_confidence},
        {"target_fill_rate", c.target_fill_rate},
        {"enable_s1", c.enable_s1},
        {"enable_s2", c.enable_s2},
        {"enable_s3", c.enable_s3}
    };
}

void from_json(const nlohmann::json& j, StrategyConfig& c) {
    if (j.contains("min_edge_cents")) j.at("min_edge_cents").get_to(c.min_edge_cents);
    if (j.contains("max_spread_to_trade")) j.at("max_spread_to_trade").get_to(c.max_spread_to_trade);
    if (j.contains("lag_move_threshold_bps")) j.at("lag_move_threshold_bps").get_to(c.lag_move_threshold_bps);
    if (j.contains("staleness_window_ms")) j.at("staleness_window_ms").get_to(c.staleness_window_ms);
    if (j.contains("min_confidence")) j.at("min_confidence").get_to(c.min_confidence);
    if (j.contains("target_fill_rate")) j.at("target_fill_rate").get_to(c.target_fill_rate);
    if (j.contains("enable_s1")) j.at("enable_s1").get_to(c.enable_s1);
    if (j.contains("enable_s2")) j.at("enable_s2").get_to(c.enable_s2);
    if (j.contains("enable_s3")) j.at("enable_s3").get_to(c.enable_s3);
}

void to_json(nlohmann::json& j, const ConnectionConfig& c) {
    j = nlohmann::json{
        {"polymarket_rest_url", c.polymarket_rest_url},
        {"polymarket_ws_url", c.polymarket_ws_url},
        {"polymarket_gamma_url", c.polymarket_gamma_url},
        {"binance_ws_url", c.binance_ws_url},
        {"binance_symbol", c.binance_symbol},
        {"reconnect_delay_ms", c.reconnect_delay_ms},
        {"max_reconnect_attempts", c.max_reconnect_attempts},
        {"heartbeat_interval_ms", c.heartbeat_interval_ms},
        {"connection_timeout_ms", c.connection_timeout_ms}
    };
}

void from_json(const nlohmann::json& j, ConnectionConfig& c) {
    if (j.contains("polymarket_rest_url")) j.at("polymarket_rest_url").get_to(c.polymarket_rest_url);
    if (j.contains("polymarket_ws_url")) j.at("polymarket_ws_url").get_to(c.polymarket_ws_url);
    if (j.contains("polymarket_gamma_url")) j.at("polymarket_gamma_url").get_to(c.polymarket_gamma_url);
    if (j.contains("binance_ws_url")) j.at("binance_ws_url").get_to(c.binance_ws_url);
    if (j.contains("binance_symbol")) j.at("binance_symbol").get_to(c.binance_symbol);
    if (j.contains("reconnect_delay_ms")) j.at("reconnect_delay_ms").get_to(c.reconnect_delay_ms);
    if (j.contains("max_reconnect_attempts")) j.at("max_reconnect_attempts").get_to(c.max_reconnect_attempts);
    if (j.contains("heartbeat_interval_ms")) j.at("heartbeat_interval_ms").get_to(c.heartbeat_interval_ms);
    if (j.contains("connection_timeout_ms")) j.at("connection_timeout_ms").get_to(c.connection_timeout_ms);
}

void to_json(nlohmann::json& j, const LoggingConfig& c) {
    j = nlohmann::json{
        {"log_dir", c.log_dir},
        {"log_level", c.log_level},
        {"log_to_console", c.log_to_console},
        {"log_to_file", c.log_to_file},
        {"json_format", c.json_format},
        {"max_log_file_size_mb", c.max_log_file_size_mb},
        {"max_log_files", c.max_log_files}
    };
}

void from_json(const nlohmann::json& j, LoggingConfig& c) {
    if (j.contains("log_dir")) j.at("log_dir").get_to(c.log_dir);
    if (j.contains("log_level")) j.at("log_level").get_to(c.log_level);
    if (j.contains("log_to_console")) j.at("log_to_console").get_to(c.log_to_console);
    if (j.contains("log_to_file")) j.at("log_to_file").get_to(c.log_to_file);
    if (j.contains("json_format")) j.at("json_format").get_to(c.json_format);
    if (j.contains("max_log_file_size_mb")) j.at("max_log_file_size_mb").get_to(c.max_log_file_size_mb);
    if (j.contains("max_log_files")) j.at("max_log_files").get_to(c.max_log_files);
}

void to_json(nlohmann::json& j, const Config& c) {
    std::string mode_str;
    switch (c.mode) {
        case TradingMode::DRY_RUN: mode_str = "dry-run"; break;
        case TradingMode::PAPER: mode_str = "paper"; break;
        case TradingMode::LIVE: mode_str = "live"; break;
    }

    j = nlohmann::json{
        {"mode", mode_str},
        {"starting_balance_usdc", c.starting_balance_usdc},
        {"risk", c.risk},
        {"strategy", c.strategy},
        {"connection", c.connection},
        {"logging", c.logging},
        {"trade_ledger_path", c.trade_ledger_path},
        {"state_snapshot_path", c.state_snapshot_path},
        {"market_slugs", c.market_slugs},
        {"market_pattern", c.market_pattern}
    };
}

void from_json(const nlohmann::json& j, Config& c) {
    if (j.contains("mode")) {
        std::string mode_str = j.at("mode").get<std::string>();
        if (mode_str == "dry-run" || mode_str == "dry_run") c.mode = TradingMode::DRY_RUN;
        else if (mode_str == "paper") c.mode = TradingMode::PAPER;
        else if (mode_str == "live") c.mode = TradingMode::LIVE;
    }
    if (j.contains("starting_balance_usdc")) j.at("starting_balance_usdc").get_to(c.starting_balance_usdc);
    if (j.contains("risk")) j.at("risk").get_to(c.risk);
    if (j.contains("strategy")) j.at("strategy").get_to(c.strategy);
    if (j.contains("connection")) j.at("connection").get_to(c.connection);
    if (j.contains("logging")) j.at("logging").get_to(c.logging);
    if (j.contains("trade_ledger_path")) j.at("trade_ledger_path").get_to(c.trade_ledger_path);
    if (j.contains("state_snapshot_path")) j.at("state_snapshot_path").get_to(c.state_snapshot_path);
    if (j.contains("market_slugs")) j.at("market_slugs").get_to(c.market_slugs);
    if (j.contains("market_pattern")) j.at("market_pattern").get_to(c.market_pattern);
}

Config Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open config file: " + path);
    }

    nlohmann::json j;
    file >> j;

    Config config;
    from_json(j, config);

    if (!config.validate()) {
        throw std::runtime_error("Invalid configuration in: " + path);
    }

    return config;
}

void Config::save(const std::string& path) const {
    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to create config file: " + path);
    }

    nlohmann::json j;
    to_json(j, *this);
    file << j.dump(2);
}

bool Config::validate() const {
    // Basic validation
    if (starting_balance_usdc <= 0) {
        spdlog::error("starting_balance_usdc must be positive");
        return false;
    }

    if (risk.max_notional_per_trade <= 0) {
        spdlog::error("max_notional_per_trade must be positive");
        return false;
    }

    if (risk.max_notional_per_trade > starting_balance_usdc * 0.5) {
        spdlog::warn("max_notional_per_trade is > 50% of balance, this is risky");
    }

    if (risk.max_daily_loss <= 0 || risk.max_daily_loss > starting_balance_usdc) {
        spdlog::error("max_daily_loss must be positive and <= balance");
        return false;
    }

    if (strategy.min_edge_cents < 0) {
        spdlog::error("min_edge_cents must be non-negative");
        return false;
    }

    return true;
}

std::string Config::get_env(const std::string& name, const std::string& default_val) {
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : default_val;
}

} // namespace arb
