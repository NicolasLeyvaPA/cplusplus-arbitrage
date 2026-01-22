#include "core/degradation_manager.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace arb {

TradingRestrictions TradingRestrictions::for_mode(OperatingMode mode) {
    TradingRestrictions r;

    switch (mode) {
        case OperatingMode::NORMAL:
            // No restrictions
            break;

        case OperatingMode::REDUCED:
            r.max_position_size_multiplier = 0.5;
            r.min_edge_multiplier = 1.5;
            r.max_exposure_multiplier = 0.75;
            r.max_concurrent_orders = 5;
            r.min_order_interval = std::chrono::milliseconds(100);
            break;

        case OperatingMode::MINIMAL:
            r.allow_new_positions = false;
            r.allow_aggressive_orders = false;
            r.max_position_size_multiplier = 0.25;
            r.min_edge_multiplier = 2.0;
            r.max_exposure_multiplier = 0.5;
            r.max_concurrent_orders = 2;
            r.min_order_interval = std::chrono::milliseconds(500);
            break;

        case OperatingMode::MAINTENANCE:
            r.allow_new_positions = false;
            r.allow_position_increase = false;
            r.allow_aggressive_orders = false;
            r.allow_passive_orders = false;
            r.max_position_size_multiplier = 0.0;
            r.max_concurrent_orders = 0;
            break;

        case OperatingMode::HALTED:
            r.allow_new_positions = false;
            r.allow_position_increase = false;
            r.allow_aggressive_orders = false;
            r.allow_passive_orders = false;
            r.max_position_size_multiplier = 0.0;
            r.max_concurrent_orders = 0;
            break;
    }

    return r;
}

DegradationManager::DegradationManager(
    std::shared_ptr<ConnectionHealthMonitor> health_monitor,
    double starting_balance,
    const Config& config
)
    : health_monitor_(std::move(health_monitor))
    , starting_balance_(starting_balance)
    , config_(config)
    , current_balance_(starting_balance)
    , last_mode_change_(wall_now())
{
    spdlog::info("DegradationManager initialized: starting_balance=${:.2f}", starting_balance);
}

void DegradationManager::update_balance(double current_balance) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_balance_ = current_balance;
}

void DegradationManager::update_daily_pnl(double daily_pnl) {
    std::lock_guard<std::mutex> lock(mutex_);
    daily_pnl_ = daily_pnl;
}

void DegradationManager::update_btc_volatility(double recent_move_percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    btc_volatility_ = std::abs(recent_move_percent);
}

void DegradationManager::record_error(const std::string& error_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_errors_.emplace_back(wall_now(), error_type);
    cleanup_old_errors();
    consecutive_healthy_checks_ = 0;
}

void DegradationManager::record_success() {
    std::lock_guard<std::mutex> lock(mutex_);
    consecutive_healthy_checks_++;
}

void DegradationManager::evaluate() {
    OperatingMode current = mode_.load();
    OperatingMode target;
    std::vector<std::string> triggers;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_old_errors();
        target = determine_mode();
        triggers = get_degradation_triggers();
    }

    if (target != current) {
        // Check cooldown for upgrades
        if (target < current) {  // Lower enum value = better mode
            if (!can_upgrade()) {
                return;  // Still in cooldown
            }
        }

        transition_mode(target, "automatic", triggers);
    }
}

TradingRestrictions DegradationManager::current_restrictions() const {
    return TradingRestrictions::for_mode(mode_.load());
}

bool DegradationManager::can_open_position() const {
    auto restrictions = current_restrictions();
    return restrictions.allow_new_positions;
}

bool DegradationManager::can_place_order() const {
    auto restrictions = current_restrictions();
    return restrictions.allow_aggressive_orders || restrictions.allow_passive_orders;
}

double DegradationManager::adjusted_max_size(double base_size) const {
    auto restrictions = current_restrictions();
    return base_size * restrictions.max_position_size_multiplier;
}

double DegradationManager::adjusted_min_edge(double base_edge) const {
    auto restrictions = current_restrictions();
    return base_edge * restrictions.min_edge_multiplier;
}

bool DegradationManager::set_mode(OperatingMode mode, const std::string& reason) {
    if (!config_.allow_manual_override) {
        spdlog::warn("Manual mode override not allowed");
        return false;
    }

    transition_mode(mode, "manual: " + reason, {"manual_override"});
    return true;
}

bool DegradationManager::upgrade_mode() {
    OperatingMode current = mode_.load();

    if (current == OperatingMode::NORMAL) {
        return true;  // Already at best
    }

    if (!can_upgrade()) {
        spdlog::debug("Cannot upgrade: cooldown or conditions not met");
        return false;
    }

    OperatingMode target;
    switch (current) {
        case OperatingMode::HALTED:
            target = OperatingMode::MAINTENANCE;
            break;
        case OperatingMode::MAINTENANCE:
            target = OperatingMode::MINIMAL;
            break;
        case OperatingMode::MINIMAL:
            target = OperatingMode::REDUCED;
            break;
        case OperatingMode::REDUCED:
            target = OperatingMode::NORMAL;
            break;
        default:
            return true;
    }

    transition_mode(target, "conditions improved", {"recovery"});
    return true;
}

bool DegradationManager::downgrade_mode(const std::string& reason) {
    OperatingMode current = mode_.load();

    if (current == OperatingMode::HALTED) {
        return true;  // Already at worst
    }

    OperatingMode target;
    switch (current) {
        case OperatingMode::NORMAL:
            target = OperatingMode::REDUCED;
            break;
        case OperatingMode::REDUCED:
            target = OperatingMode::MINIMAL;
            break;
        case OperatingMode::MINIMAL:
            target = OperatingMode::MAINTENANCE;
            break;
        case OperatingMode::MAINTENANCE:
            target = OperatingMode::HALTED;
            break;
        default:
            return true;
    }

    transition_mode(target, reason, {"manual_downgrade"});
    return true;
}

std::vector<DegradationEvent> DegradationManager::get_event_history() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return event_history_;
}

std::string DegradationManager::status_summary() const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto restrictions = TradingRestrictions::for_mode(mode_.load());

    return fmt::format(
        "Mode: {} | New positions: {} | Size mult: {:.1f}x | Edge mult: {:.1f}x | Errors: {}",
        mode_to_string(mode_.load()),
        restrictions.allow_new_positions ? "YES" : "NO",
        restrictions.max_position_size_multiplier,
        restrictions.min_edge_multiplier,
        error_count()
    );
}

OperatingMode DegradationManager::determine_mode() const {
    // Check connection health first
    if (health_monitor_) {
        auto health = health_monitor_->system_health();

        if (health.overall_status == HealthStatus::DISCONNECTED) {
            return OperatingMode::HALTED;
        }

        if (health.overall_status == HealthStatus::UNHEALTHY) {
            return OperatingMode::MAINTENANCE;
        }

        // Check specific required connections
        if (config_.require_binance) {
            auto binance = health.connections.find("binance");
            if (binance == health.connections.end() ||
                binance->second.status == HealthStatus::DISCONNECTED) {
                return OperatingMode::HALTED;
            }
        }

        if (config_.require_polymarket_ws) {
            auto poly_ws = health.connections.find("polymarket_ws");
            if (poly_ws == health.connections.end() ||
                poly_ws->second.status == HealthStatus::DISCONNECTED) {
                return OperatingMode::MAINTENANCE;
            }
        }
    }

    // Check loss thresholds
    double loss_percent = (starting_balance_ - current_balance_) / starting_balance_;

    if (loss_percent >= config_.halt_loss_percent) {
        return OperatingMode::HALTED;
    }

    if (loss_percent >= config_.minimal_mode_loss_percent) {
        return OperatingMode::MINIMAL;
    }

    if (loss_percent >= config_.reduced_mode_loss_percent) {
        return OperatingMode::REDUCED;
    }

    // Check volatility
    if (btc_volatility_ >= config_.minimal_mode_volatility) {
        return OperatingMode::MINIMAL;
    }

    if (btc_volatility_ >= config_.reduced_mode_volatility) {
        return OperatingMode::REDUCED;
    }

    // Check error count
    int errors = error_count();

    if (errors >= config_.halt_errors) {
        return OperatingMode::HALTED;
    }

    if (errors >= config_.minimal_mode_errors) {
        return OperatingMode::MINIMAL;
    }

    if (errors >= config_.reduced_mode_errors) {
        return OperatingMode::REDUCED;
    }

    return OperatingMode::NORMAL;
}

std::vector<std::string> DegradationManager::get_degradation_triggers() const {
    std::vector<std::string> triggers;

    // Connection issues
    if (health_monitor_) {
        auto health = health_monitor_->system_health();
        for (const auto& name : health.unhealthy_connections()) {
            triggers.push_back("unhealthy_connection:" + name);
        }
    }

    // Loss threshold
    double loss_percent = (starting_balance_ - current_balance_) / starting_balance_;
    if (loss_percent > 0) {
        triggers.push_back(fmt::format("loss:{:.1f}%", loss_percent * 100));
    }

    // Volatility
    if (btc_volatility_ > config_.reduced_mode_volatility) {
        triggers.push_back(fmt::format("volatility:{:.1f}%", btc_volatility_ * 100));
    }

    // Errors
    int errors = error_count();
    if (errors > 0) {
        triggers.push_back(fmt::format("errors:{}", errors));
    }

    return triggers;
}

bool DegradationManager::can_upgrade() const {
    // Check cooldown
    auto since_change = wall_now() - last_mode_change_;
    if (since_change < config_.recovery_cooldown) {
        return false;
    }

    // Check consecutive healthy checks
    if (consecutive_healthy_checks_ < config_.required_healthy_checks) {
        return false;
    }

    return true;
}

void DegradationManager::transition_mode(
    OperatingMode new_mode,
    const std::string& reason,
    const std::vector<std::string>& triggers
) {
    OperatingMode old_mode = mode_.exchange(new_mode);

    if (old_mode == new_mode) {
        return;  // No change
    }

    last_mode_change_ = wall_now();
    consecutive_healthy_checks_ = 0;

    // Record event
    DegradationEvent event{
        wall_now(),
        old_mode,
        new_mode,
        reason,
        triggers
    };

    {
        std::lock_guard<std::mutex> lock(mutex_);
        event_history_.push_back(event);

        // Keep history bounded
        if (event_history_.size() > 1000) {
            event_history_.erase(event_history_.begin(), event_history_.begin() + 500);
        }
    }

    spdlog::warn("Operating mode changed: {} -> {} ({})",
                mode_to_string(old_mode),
                mode_to_string(new_mode),
                reason);

    for (const auto& trigger : triggers) {
        spdlog::info("  Trigger: {}", trigger);
    }

    // Invoke callback
    if (on_mode_change_) {
        try {
            on_mode_change_(old_mode, new_mode, reason);
        } catch (const std::exception& e) {
            spdlog::error("Mode change callback error: {}", e.what());
        }
    }
}

void DegradationManager::cleanup_old_errors() {
    auto cutoff = wall_now() - config_.error_window;

    recent_errors_.erase(
        std::remove_if(recent_errors_.begin(), recent_errors_.end(),
                       [&cutoff](const auto& e) { return e.first < cutoff; }),
        recent_errors_.end()
    );
}

int DegradationManager::error_count() const {
    return static_cast<int>(recent_errors_.size());
}

} // namespace arb
