#include "risk/risk_manager.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace arb {

RiskManager::RiskManager(const RiskConfig& config, double starting_balance)
    : config_(config)
    , starting_balance_(starting_balance)
    , current_balance_(starting_balance)
{
    spdlog::info("RiskManager initialized with balance=${:.2f}, max_loss=${:.2f}",
                 starting_balance, config.max_daily_loss);
}

RiskManager::CheckResult RiskManager::check_order(const Signal& signal, Notional notional) const {
    CheckResult result;

    // Check kill switch
    if (kill_switch_.load()) {
        result.reason = "Kill switch active: " + kill_switch_reason();
        return result;
    }

    // Check max notional per trade
    if (notional > config_.max_notional_per_trade) {
        result.reason = fmt::format("Notional ${:.2f} exceeds max ${:.2f}",
                                    notional, config_.max_notional_per_trade);
        return result;
    }

    // Check daily loss limit
    if (daily_pnl_.load() <= -config_.max_daily_loss) {
        result.reason = fmt::format("Daily loss limit reached: ${:.2f}",
                                    -daily_pnl_.load());
        return result;
    }

    // Check position limit
    auto pos_check = check_position_limit(signal.market_id);
    if (!pos_check.allowed) {
        return pos_check;
    }

    // Check available balance
    if (notional > available_balance()) {
        result.reason = fmt::format("Insufficient balance: need ${:.2f}, have ${:.2f}",
                                    notional, available_balance());
        return result;
    }

    result.allowed = true;
    return result;
}

RiskManager::CheckResult RiskManager::check_position_limit(const std::string& market_id) const {
    CheckResult result;

    std::lock_guard<std::mutex> lock(position_mutex_);

    // Check max open positions
    if (open_positions_ >= static_cast<int>(config_.max_open_positions)) {
        result.reason = fmt::format("Max open positions reached: {}",
                                    config_.max_open_positions);
        return result;
    }

    // Check exposure per market
    auto it = market_exposure_.find(market_id);
    if (it != market_exposure_.end()) {
        if (it->second >= config_.max_exposure_per_market) {
            result.reason = fmt::format("Market exposure limit reached for {}: ${:.2f}",
                                        market_id, config_.max_exposure_per_market);
            return result;
        }
    }

    result.allowed = true;
    return result;
}

RiskManager::CheckResult RiskManager::check_daily_loss() const {
    CheckResult result;

    double remaining = daily_loss_remaining();
    if (remaining <= 0) {
        result.reason = "Daily loss limit reached";
        return result;
    }

    result.allowed = true;
    return result;
}

void RiskManager::record_fill(const Fill& fill) {
    std::lock_guard<std::mutex> lock(position_mutex_);

    // Update market exposure
    double notional = fill.size * fill.price;
    if (fill.side == Side::BUY) {
        market_exposure_[fill.market_id] += notional;
        open_positions_++;
    } else {
        market_exposure_[fill.market_id] -= notional;
        if (market_exposure_[fill.market_id] <= 0) {
            market_exposure_.erase(fill.market_id);
            open_positions_ = std::max(0, open_positions_ - 1);
        }
    }

    spdlog::debug("Position update: market={}, exposure=${:.2f}, open_positions={}",
                  fill.market_id, market_exposure_[fill.market_id], open_positions_);
}

void RiskManager::record_pnl(double realized_pnl) {
    double old_daily = daily_pnl_.load();
    daily_pnl_.store(old_daily + realized_pnl);

    double new_balance = current_balance_.load() + realized_pnl;
    current_balance_.store(new_balance);

    spdlog::info("PnL recorded: ${:.2f}, Daily PnL: ${:.2f}, Balance: ${:.2f}",
                 realized_pnl, daily_pnl_.load(), new_balance);

    // Check stop loss threshold
    if ((starting_balance_ - new_balance) / starting_balance_ >= config_.stop_loss_threshold) {
        activate_kill_switch("Stop loss threshold exceeded");
    }
}

double RiskManager::current_exposure() const {
    std::lock_guard<std::mutex> lock(position_mutex_);
    double total = 0.0;
    for (const auto& [market, exposure] : market_exposure_) {
        total += exposure;
    }
    return total;
}

double RiskManager::exposure_for_market(const std::string& market_id) const {
    std::lock_guard<std::mutex> lock(position_mutex_);
    auto it = market_exposure_.find(market_id);
    return (it != market_exposure_.end()) ? it->second : 0.0;
}

int RiskManager::open_position_count() const {
    std::lock_guard<std::mutex> lock(position_mutex_);
    return open_positions_;
}

double RiskManager::daily_loss_remaining() const {
    return config_.max_daily_loss + daily_pnl_.load();  // daily_pnl is negative for losses
}

void RiskManager::activate_kill_switch(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(kill_switch_mutex_);
        kill_switch_reason_ = reason;
    }
    kill_switch_.store(true);
    spdlog::warn("KILL SWITCH ACTIVATED: {}", reason);
}

void RiskManager::deactivate_kill_switch() {
    kill_switch_.store(false);
    {
        std::lock_guard<std::mutex> lock(kill_switch_mutex_);
        kill_switch_reason_.clear();
    }
    spdlog::info("Kill switch deactivated");
}

std::string RiskManager::kill_switch_reason() const {
    std::lock_guard<std::mutex> lock(kill_switch_mutex_);
    return kill_switch_reason_;
}

void RiskManager::record_slippage(double slippage_bps) {
    std::lock_guard<std::mutex> lock(slippage_mutex_);

    recent_slippage_.push_back({now(), slippage_bps});

    while (recent_slippage_.size() > MAX_SLIPPAGE_SAMPLES) {
        recent_slippage_.pop_front();
    }

    // Check if slippage is too high
    if (slippage_bps > config_.slippage_threshold_bps) {
        spdlog::warn("High slippage detected: {:.1f}bps", slippage_bps);

        // Count recent high-slippage events
        int high_slippage_count = 0;
        for (const auto& [ts, slip] : recent_slippage_) {
            if (slip > config_.slippage_threshold_bps) {
                high_slippage_count++;
            }
        }

        if (high_slippage_count >= 5) {
            activate_kill_switch("Excessive slippage detected");
        }
    }
}

void RiskManager::record_connectivity_issue() {
    connectivity_issues_++;
    last_connectivity_issue_ = now();

    if (connectivity_issues_.load() >= 10) {
        activate_kill_switch("Connectivity issues detected");
    }
}

bool RiskManager::should_halt_trading() const {
    if (kill_switch_.load()) return true;

    // Check recent connectivity
    if (connectivity_issues_.load() >= 5) {
        auto time_since = now() - last_connectivity_issue_;
        if (time_since < std::chrono::minutes(1)) {
            return true;
        }
    }

    return false;
}

bool RiskManager::can_place_order() {
    std::lock_guard<std::mutex> lock(rate_limit_mutex_);

    auto now_time = now();

    // Remove old timestamps
    while (!order_timestamps_.empty()) {
        auto age = now_time - order_timestamps_.front();
        if (age > std::chrono::minutes(1)) {
            order_timestamps_.pop_front();
        } else {
            break;
        }
    }

    // Check rate limit
    if (static_cast<int>(order_timestamps_.size()) >= config_.max_orders_per_minute) {
        spdlog::warn("Rate limit reached: {} orders/min", config_.max_orders_per_minute);
        return false;
    }

    return true;
}

void RiskManager::record_order_placed() {
    std::lock_guard<std::mutex> lock(rate_limit_mutex_);
    order_timestamps_.push_back(now());
}

void RiskManager::reset_daily_counters() {
    daily_pnl_.store(0.0);

    {
        std::lock_guard<std::mutex> lock(slippage_mutex_);
        recent_slippage_.clear();
    }

    connectivity_issues_.store(0);

    spdlog::info("Daily counters reset");
}

double RiskManager::available_balance() const {
    return current_balance_.load() - current_exposure();
}

void RiskManager::update_balance(double new_balance) {
    current_balance_.store(new_balance);
}

} // namespace arb
