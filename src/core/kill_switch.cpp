#include "core/kill_switch.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace arb {

KillSwitch::KillSwitch(double starting_balance, const Config& config)
    : starting_balance_(starting_balance)
    , config_(config)
{
    spdlog::info("KillSwitch initialized: starting_balance=${:.2f}, daily_limit=${:.2f}",
                 starting_balance, config.daily_loss_limit);
}

KillReason KillSwitch::reason() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_reason_;
}

std::string KillSwitch::details() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_details_;
}

WallClock KillSwitch::activation_time() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return activation_time_;
}

void KillSwitch::activate(KillReason reason, const std::string& details) {
    // Use compare_exchange to ensure we only activate once
    bool expected = false;
    if (!active_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        // Already active, just log
        spdlog::debug("KillSwitch already active, ignoring activation: {}", details);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_reason_ = reason;
        current_details_ = details;
        activation_time_ = wall_now();
    }

    record_event(reason, details, true);

    spdlog::critical("KILL SWITCH ACTIVATED: reason={}, details={}",
                     kill_reason_to_string(reason), details);

    invoke_callback(reason, details);
}

void KillSwitch::activate_manual(const std::string& operator_note) {
    activate(KillReason::MANUAL, operator_note.empty() ? "Manual activation" : operator_note);
}

bool KillSwitch::deactivate(const std::string& operator_note) {
    bool expected = true;
    if (!active_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        spdlog::warn("KillSwitch deactivation requested but not active");
        return false;
    }

    KillReason prev_reason;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        prev_reason = current_reason_;
        current_reason_ = KillReason::UNKNOWN;
        current_details_.clear();
    }

    record_event(prev_reason, operator_note, false);

    spdlog::warn("KILL SWITCH DEACTIVATED by operator: {}", operator_note);

    // Reset rate limit counter on deactivation
    rate_limit_breaches_.store(0);

    return true;
}

bool KillSwitch::check_daily_loss(double current_daily_pnl) {
    if (is_active()) return false;

    // Soft limit from config
    if (current_daily_pnl <= -config_.daily_loss_limit) {
        activate(KillReason::DAILY_LOSS_LIMIT,
                 fmt::format("Daily loss ${:.2f} exceeded limit ${:.2f}",
                            -current_daily_pnl, config_.daily_loss_limit));
        return true;
    }

    return false;
}

bool KillSwitch::check_total_loss(double current_balance) {
    if (is_active()) return false;

    double loss_percent = (starting_balance_ - current_balance) / starting_balance_;

    // Hard limit (absolute)
    if (loss_percent >= ABSOLUTE_MAX_LOSS_PERCENT) {
        activate(KillReason::TOTAL_LOSS_LIMIT,
                 fmt::format("HARD LIMIT: Lost {:.1f}% of starting balance (limit {:.1f}%)",
                            loss_percent * 100, ABSOLUTE_MAX_LOSS_PERCENT * 100));
        return true;
    }

    // Soft limit from config
    if (loss_percent >= config_.total_loss_limit_percent) {
        activate(KillReason::TOTAL_LOSS_LIMIT,
                 fmt::format("Lost {:.1f}% of starting balance (limit {:.1f}%)",
                            loss_percent * 100, config_.total_loss_limit_percent * 100));
        return true;
    }

    return false;
}

bool KillSwitch::check_exposure(double current_exposure) {
    if (is_active()) return false;

    // Hard limit (absolute)
    if (current_exposure >= ABSOLUTE_MAX_EXPOSURE) {
        activate(KillReason::EXPOSURE_BREACH,
                 fmt::format("HARD LIMIT: Exposure ${:.2f} exceeded absolute max ${:.2f}",
                            current_exposure, ABSOLUTE_MAX_EXPOSURE));
        return true;
    }

    // Soft limit from config
    if (current_exposure >= config_.max_exposure) {
        activate(KillReason::EXPOSURE_BREACH,
                 fmt::format("Exposure ${:.2f} exceeded limit ${:.2f}",
                            current_exposure, config_.max_exposure));
        return true;
    }

    return false;
}

bool KillSwitch::check_position_count(int open_positions) {
    if (is_active()) return false;

    if (open_positions >= ABSOLUTE_MAX_OPEN_POSITIONS) {
        activate(KillReason::EXPOSURE_BREACH,
                 fmt::format("HARD LIMIT: {} positions exceeded max {}",
                            open_positions, ABSOLUTE_MAX_OPEN_POSITIONS));
        return true;
    }

    return false;
}

bool KillSwitch::check_connectivity(int consecutive_failures) {
    if (is_active()) return false;

    if (consecutive_failures >= config_.max_connectivity_failures) {
        activate(KillReason::CONNECTIVITY_LOSS,
                 fmt::format("{} consecutive connection failures", consecutive_failures));
        return true;
    }

    return false;
}

bool KillSwitch::check_slippage(double slippage_bps) {
    if (is_active()) return false;

    if (slippage_bps < config_.high_slippage_bps) {
        return false;
    }

    // Record high slippage event
    {
        std::lock_guard<std::mutex> lock(slippage_mutex_);
        slippage_events_.emplace_back(wall_now(), slippage_bps);
        cleanup_old_slippage_events();

        // Count events in window
        int count = static_cast<int>(slippage_events_.size());
        if (count >= config_.max_slippage_events) {
            activate(KillReason::HIGH_SLIPPAGE,
                     fmt::format("{} high-slippage events (>{:.0f}bps) in {} minutes",
                                count, config_.high_slippage_bps, config_.slippage_window.count()));
            return true;
        }
    }

    spdlog::warn("High slippage detected: {:.1f}bps", slippage_bps);
    return false;
}

bool KillSwitch::check_rate_limit_breach() {
    if (is_active()) return false;

    int breaches = ++rate_limit_breaches_;
    if (breaches >= MAX_RATE_LIMIT_BREACHES) {
        activate(KillReason::RATE_LIMIT_BREACH,
                 fmt::format("{} rate limit breaches", breaches));
        return true;
    }

    spdlog::warn("Rate limit breach #{}", breaches);
    return false;
}

std::vector<KillEvent> KillSwitch::get_event_history() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    return event_history_;
}

void KillSwitch::clear_history() {
    std::lock_guard<std::mutex> lock(history_mutex_);
    event_history_.clear();
}

bool KillSwitch::would_breach_absolute_limits(
    double current_balance,
    double starting_balance,
    double exposure,
    int positions
) {
    double loss_percent = (starting_balance - current_balance) / starting_balance;

    if (loss_percent >= ABSOLUTE_MAX_LOSS_PERCENT) return true;
    if (exposure >= ABSOLUTE_MAX_EXPOSURE) return true;
    if (positions >= ABSOLUTE_MAX_OPEN_POSITIONS) return true;

    return false;
}

void KillSwitch::record_event(KillReason reason, const std::string& details, bool is_activation) {
    KillEvent event{
        wall_now(),
        reason,
        details,
        is_activation
    };

    std::lock_guard<std::mutex> lock(history_mutex_);
    event_history_.push_back(event);

    // Keep history bounded
    if (event_history_.size() > 1000) {
        event_history_.erase(event_history_.begin(), event_history_.begin() + 500);
    }
}

void KillSwitch::invoke_callback(KillReason reason, const std::string& details) {
    if (callback_) {
        try {
            callback_(reason, details);
        } catch (const std::exception& e) {
            spdlog::error("KillSwitch callback error: {}", e.what());
        }
    }
}

void KillSwitch::cleanup_old_slippage_events() {
    auto cutoff = wall_now() - config_.slippage_window;

    slippage_events_.erase(
        std::remove_if(slippage_events_.begin(), slippage_events_.end(),
                       [&cutoff](const auto& event) { return event.first < cutoff; }),
        slippage_events_.end()
    );
}

} // namespace arb
