#include "risk/risk_manager.hpp"
#include <spdlog/spdlog.h>
#include <cmath>

namespace arb {

RiskManager::RiskManager(const RiskConfig& config) : config_(config) {}

RiskManager::RiskCheck RiskManager::check_new_position(
    const std::string& symbol, Notional exposure, const PositionManager& positions)
{
    if (kill_switch_.load()) {
        return {false, "Kill switch active: " + kill_reason()};
    }

    // Check total exposure
    double current_total = positions.total_exposure();
    if (current_total + exposure > config_.max_total_exposure_usd) {
        return {false, fmt::format("Total exposure ${:.0f} + ${:.0f} exceeds limit ${:.0f}",
                                    current_total, exposure, config_.max_total_exposure_usd)};
    }

    // Check per-symbol exposure
    auto pos = positions.get_position(symbol);
    double symbol_exposure = pos ? pos->total_notional() : 0;
    if (symbol_exposure + exposure > config_.max_per_symbol_exposure_usd) {
        return {false, fmt::format("{} exposure ${:.0f} + ${:.0f} exceeds limit ${:.0f}",
                                    symbol, symbol_exposure, exposure,
                                    config_.max_per_symbol_exposure_usd)};
    }

    // Check max positions
    if (!pos && positions.open_position_count() >= config_.max_total_exposure_usd / config_.max_per_symbol_exposure_usd) {
        return {false, "Max position count reached"};
    }

    return {true, ""};
}

RiskManager::RiskCheck RiskManager::check_daily_loss(double daily_pnl) {
    if (daily_pnl < -config_.max_daily_loss_usd) {
        return {false, fmt::format("Daily loss ${:.2f} exceeds limit ${:.0f}",
                                    daily_pnl, config_.max_daily_loss_usd)};
    }
    return {true, ""};
}

RiskManager::RiskCheck RiskManager::check_drawdown(double current_equity, double high_water_mark) {
    if (high_water_mark <= 0) return {true, ""};
    double drawdown_pct = (high_water_mark - current_equity) / high_water_mark * 100.0;
    if (drawdown_pct > config_.max_drawdown_pct) {
        return {false, fmt::format("Drawdown {:.2f}% exceeds limit {:.1f}%",
                                    drawdown_pct, config_.max_drawdown_pct)};
    }
    return {true, ""};
}

RiskManager::RiskCheck RiskManager::check_funding_rate(double funding_rate) {
    if (funding_rate < config_.max_funding_rate_to_pay) {
        return {false, fmt::format("Funding rate {:.4f}% below payment threshold {:.4f}%",
                                    funding_rate * 100, config_.max_funding_rate_to_pay * 100)};
    }
    return {true, ""};
}

RiskManager::RiskCheck RiskManager::check_basis_risk(double basis_bps) {
    if (std::abs(basis_bps) > config_.stop_loss_basis_bps) {
        return {false, fmt::format("Basis {:.2f}bps exceeds limit {:.0f}bps",
                                    basis_bps, config_.stop_loss_basis_bps)};
    }
    return {true, ""};
}

void RiskManager::activate_kill_switch(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    kill_switch_ = true;
    kill_reason_ = reason;
    spdlog::error("KILL SWITCH ACTIVATED: {}", reason);
}

void RiskManager::deactivate_kill_switch() {
    std::lock_guard<std::mutex> lock(mutex_);
    kill_switch_ = false;
    kill_reason_.clear();
    spdlog::info("Kill switch deactivated");
}

std::string RiskManager::kill_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kill_reason_;
}

void RiskManager::record_daily_pnl(double pnl) {
    std::lock_guard<std::mutex> lock(mutex_);
    daily_pnl_ = pnl;
}

void RiskManager::reset_daily() {
    std::lock_guard<std::mutex> lock(mutex_);
    daily_pnl_ = 0;
}

} // namespace arb
