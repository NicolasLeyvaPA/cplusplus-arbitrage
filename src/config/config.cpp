#include "config/config.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fstream>
#include <cstdlib>
#include <stdexcept>

namespace arb {

std::string Config::get_env(const std::string& name, const std::string& default_val) {
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : default_val;
}

Config Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        spdlog::warn("Config file not found: {}, using defaults", path);
        return Config{};
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to parse config: " + std::string(e.what()));
    }

    Config config;

    if (j.contains("mode")) config.mode = trading_mode_from_str(j["mode"].get<std::string>());

    if (j.contains("exchange")) {
        auto& ex = j["exchange"];
        if (ex.contains("exchange_type")) config.exchange.exchange_type = ex["exchange_type"];
        if (ex.contains("api_key_env")) config.exchange.api_key_env = ex["api_key_env"];
        if (ex.contains("api_secret_env")) config.exchange.api_secret_env = ex["api_secret_env"];
        if (ex.contains("testnet")) config.exchange.testnet = ex["testnet"];
        if (ex.contains("spot_rest_url")) config.exchange.spot_rest_url = ex["spot_rest_url"];
        if (ex.contains("spot_ws_url")) config.exchange.spot_ws_url = ex["spot_ws_url"];
        if (ex.contains("futures_rest_url")) config.exchange.futures_rest_url = ex["futures_rest_url"];
        if (ex.contains("futures_ws_url")) config.exchange.futures_ws_url = ex["futures_ws_url"];
        if (ex.contains("wallet_private_key_env")) config.exchange.wallet_private_key_env = ex["wallet_private_key_env"];
        if (ex.contains("hl_rest_url")) config.exchange.hl_rest_url = ex["hl_rest_url"];
        if (ex.contains("hl_user_address")) config.exchange.hl_user_address = ex["hl_user_address"];
        if (ex.contains("hl_use_api_wallet")) config.exchange.hl_use_api_wallet = ex["hl_use_api_wallet"];
        if (ex.contains("reconnect_delay_ms")) config.exchange.reconnect_delay_ms = ex["reconnect_delay_ms"];
        if (ex.contains("max_reconnect_attempts")) config.exchange.max_reconnect_attempts = ex["max_reconnect_attempts"];
        if (ex.contains("connection_timeout_ms")) config.exchange.connection_timeout_ms = ex["connection_timeout_ms"];
    }

    if (j.contains("fees")) {
        auto& fe = j["fees"];
        if (fe.contains("perp_taker_fee_pct")) config.fees.perp_taker_fee_pct = fe["perp_taker_fee_pct"];
        if (fe.contains("perp_maker_fee_pct")) config.fees.perp_maker_fee_pct = fe["perp_maker_fee_pct"];
        if (fe.contains("spot_taker_fee_pct")) config.fees.spot_taker_fee_pct = fe["spot_taker_fee_pct"];
        if (fe.contains("spot_maker_fee_pct")) config.fees.spot_maker_fee_pct = fe["spot_maker_fee_pct"];
    }

    if (j.contains("strategy")) {
        auto& st = j["strategy"];
        if (st.contains("symbols")) config.strategy.symbols = st["symbols"].get<std::vector<std::string>>();
        if (st.contains("hedge_mode")) config.strategy.hedge_mode = hedge_mode_from_str(st["hedge_mode"].get<std::string>());
        if (st.contains("min_funding_rate")) config.strategy.min_funding_rate = st["min_funding_rate"];
        if (st.contains("min_annual_yield_pct")) config.strategy.min_annual_yield_pct = st["min_annual_yield_pct"];
        if (st.contains("min_avg_funding_rate")) config.strategy.min_avg_funding_rate = st["min_avg_funding_rate"];
        if (st.contains("max_basis_deviation_pct")) config.strategy.max_basis_deviation_pct = st["max_basis_deviation_pct"];
        if (st.contains("funding_rate_lookback_periods")) config.strategy.funding_rate_lookback_periods = st["funding_rate_lookback_periods"];
        if (st.contains("entry_score_threshold")) config.strategy.entry_score_threshold = st["entry_score_threshold"];
        if (st.contains("max_positions")) config.strategy.max_positions = st["max_positions"];
        if (st.contains("rebalance_threshold_pct")) config.strategy.rebalance_threshold_pct = st["rebalance_threshold_pct"];
        if (st.contains("max_holding_periods")) config.strategy.max_holding_periods = st["max_holding_periods"];
        if (st.contains("funding_periods_per_day")) config.strategy.funding_periods_per_day = st["funding_periods_per_day"];
        if (st.contains("min_notional_usd")) config.strategy.min_notional_usd = st["min_notional_usd"];
        if (st.contains("max_fee_breakeven_days")) config.strategy.max_fee_breakeven_days = st["max_fee_breakeven_days"];
        if (st.contains("hlp_allocation_pct")) config.strategy.hlp_allocation_pct = st["hlp_allocation_pct"];
    }

    if (j.contains("risk")) {
        auto& rk = j["risk"];
        if (rk.contains("max_total_exposure_usd")) config.risk.max_total_exposure_usd = rk["max_total_exposure_usd"];
        if (rk.contains("max_per_symbol_exposure_usd")) config.risk.max_per_symbol_exposure_usd = rk["max_per_symbol_exposure_usd"];
        if (rk.contains("max_drawdown_pct")) config.risk.max_drawdown_pct = rk["max_drawdown_pct"];
        if (rk.contains("max_daily_loss_usd")) config.risk.max_daily_loss_usd = rk["max_daily_loss_usd"];
        if (rk.contains("futures_leverage")) config.risk.futures_leverage = rk["futures_leverage"];
        if (rk.contains("margin_type")) config.risk.margin_type = rk["margin_type"];
        if (rk.contains("stop_loss_basis_bps")) config.risk.stop_loss_basis_bps = rk["stop_loss_basis_bps"];
        if (rk.contains("max_funding_rate_to_pay")) config.risk.max_funding_rate_to_pay = rk["max_funding_rate_to_pay"];
        if (rk.contains("max_orders_per_minute")) config.risk.max_orders_per_minute = rk["max_orders_per_minute"];
    }

    if (j.contains("capital")) {
        auto& cap = j["capital"];
        if (cap.contains("starting_balance_usd")) config.capital.starting_balance_usd = cap["starting_balance_usd"];
        if (cap.contains("allocation_pct")) config.capital.allocation_pct = cap["allocation_pct"];
    }

    if (j.contains("logging")) {
        auto& lg = j["logging"];
        if (lg.contains("log_dir")) config.logging.log_dir = lg["log_dir"];
        if (lg.contains("log_level")) config.logging.log_level = lg["log_level"];
        if (lg.contains("log_to_console")) config.logging.log_to_console = lg["log_to_console"];
        if (lg.contains("log_to_file")) config.logging.log_to_file = lg["log_to_file"];
    }

    if (j.contains("polling")) {
        auto& pl = j["polling"];
        if (pl.contains("funding_rate_interval_s")) config.polling.funding_rate_interval_s = pl["funding_rate_interval_s"];
        if (pl.contains("price_update_interval_ms")) config.polling.price_update_interval_ms = pl["price_update_interval_ms"];
        if (pl.contains("strategy_eval_interval_s")) config.polling.strategy_eval_interval_s = pl["strategy_eval_interval_s"];
        if (pl.contains("hedge_check_interval_s")) config.polling.hedge_check_interval_s = pl["hedge_check_interval_s"];
        if (pl.contains("pnl_snapshot_interval_s")) config.polling.pnl_snapshot_interval_s = pl["pnl_snapshot_interval_s"];
    }

    if (j.contains("database_path")) config.database_path = j["database_path"];
    if (j.contains("trade_ledger_path")) config.trade_ledger_path = j["trade_ledger_path"];

    spdlog::info("Config loaded from {}", path);
    return config;
}

void Config::save(const std::string& path) const {
    nlohmann::json j;
    j["mode"] = arb::to_string(mode);
    j["exchange"] = {
        {"exchange_type", exchange.exchange_type},
        {"testnet", exchange.testnet},
        {"hl_rest_url", exchange.hl_rest_url},
        {"wallet_private_key_env", exchange.wallet_private_key_env},
    };
    j["fees"] = {
        {"perp_taker_fee_pct", fees.perp_taker_fee_pct},
        {"perp_maker_fee_pct", fees.perp_maker_fee_pct},
    };
    j["strategy"] = {
        {"symbols", strategy.symbols},
        {"hedge_mode", arb::to_string(strategy.hedge_mode)},
        {"min_funding_rate", strategy.min_funding_rate},
        {"min_annual_yield_pct", strategy.min_annual_yield_pct},
        {"max_positions", strategy.max_positions},
        {"funding_periods_per_day", strategy.funding_periods_per_day},
        {"hlp_allocation_pct", strategy.hlp_allocation_pct},
    };
    j["risk"] = {
        {"max_total_exposure_usd", risk.max_total_exposure_usd},
        {"max_per_symbol_exposure_usd", risk.max_per_symbol_exposure_usd},
        {"max_drawdown_pct", risk.max_drawdown_pct},
        {"max_daily_loss_usd", risk.max_daily_loss_usd},
        {"futures_leverage", risk.futures_leverage},
    };
    j["capital"] = {
        {"starting_balance_usd", capital.starting_balance_usd},
        {"allocation_pct", capital.allocation_pct},
    };
    j["database_path"] = database_path;
    j["trade_ledger_path"] = trade_ledger_path;

    std::ofstream file(path);
    file << j.dump(2);
}

void Config::validate() const {
    if (strategy.symbols.empty()) throw std::runtime_error("No symbols configured");
    if (risk.max_total_exposure_usd <= 0) throw std::runtime_error("max_total_exposure_usd must be positive");
    if (capital.starting_balance_usd <= 0) throw std::runtime_error("starting_balance_usd must be positive");
    if (risk.futures_leverage < 1 || risk.futures_leverage > 20) throw std::runtime_error("futures_leverage must be 1-20");
}

} // namespace arb
