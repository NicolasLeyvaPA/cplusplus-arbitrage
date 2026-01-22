#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <functional>
#include "common/types.hpp"
#include "core/connection_health.hpp"

namespace arb {

/**
 * System operating mode.
 */
enum class OperatingMode {
    NORMAL,          // Full operation
    REDUCED,         // Reduced activity (wider spreads, smaller sizes)
    MINIMAL,         // Only essential operations (no new positions)
    MAINTENANCE,     // No trading, only position monitoring
    HALTED           // Complete halt
};

inline std::string mode_to_string(OperatingMode m) {
    switch (m) {
        case OperatingMode::NORMAL: return "NORMAL";
        case OperatingMode::REDUCED: return "REDUCED";
        case OperatingMode::MINIMAL: return "MINIMAL";
        case OperatingMode::MAINTENANCE: return "MAINTENANCE";
        case OperatingMode::HALTED: return "HALTED";
    }
    return "UNKNOWN";
}

/**
 * Trading restrictions based on current mode.
 */
struct TradingRestrictions {
    bool allow_new_positions{true};
    bool allow_position_increase{true};
    bool allow_aggressive_orders{true};  // Market orders, taking liquidity
    bool allow_passive_orders{true};     // Limit orders, providing liquidity

    double max_position_size_multiplier{1.0};  // 1.0 = normal, 0.5 = half size
    double min_edge_multiplier{1.0};           // 1.0 = normal, 2.0 = require 2x edge
    double max_exposure_multiplier{1.0};       // 1.0 = normal, 0.5 = half exposure

    int max_concurrent_orders{10};
    std::chrono::milliseconds min_order_interval{0};  // Rate limiting

    static TradingRestrictions for_mode(OperatingMode mode);
};

/**
 * Degradation event for audit.
 */
struct DegradationEvent {
    WallClock timestamp;
    OperatingMode from_mode;
    OperatingMode to_mode;
    std::string reason;
    std::vector<std::string> triggers;  // What caused the degradation
};

/**
 * Graceful degradation manager adjusts system behavior based on conditions.
 *
 * DESIGN:
 * - Monitors multiple health signals
 * - Automatically adjusts operating mode
 * - Reduces activity proportionally to issues
 * - Never trades beyond what conditions allow
 * - Recovers automatically when conditions improve
 */
class DegradationManager {
public:
    using ModeChangeCallback = std::function<void(OperatingMode, OperatingMode, const std::string&)>;

    struct Config {
        // Connection health thresholds
        bool require_binance{true};
        bool require_polymarket_ws{true};
        bool require_polymarket_rest{true};

        // PnL thresholds
        double reduced_mode_loss_percent{0.03};   // 3% loss -> reduced
        double minimal_mode_loss_percent{0.05};   // 5% loss -> minimal
        double halt_loss_percent{0.08};           // 8% loss -> halt

        // Volatility thresholds
        double reduced_mode_volatility{0.02};     // 2% move -> reduced
        double minimal_mode_volatility{0.05};     // 5% move -> minimal

        // Error thresholds
        int reduced_mode_errors{3};
        int minimal_mode_errors{5};
        int halt_errors{10};
        std::chrono::minutes error_window{5};

        // Recovery settings
        std::chrono::seconds recovery_cooldown{60};
        int required_healthy_checks{5};           // Consecutive healthy checks to upgrade

        // Manual override
        bool allow_manual_override{true};
    };

    explicit DegradationManager(
        std::shared_ptr<ConnectionHealthMonitor> health_monitor,
        double starting_balance,
        const Config& config = Config{}
    );

    // Update inputs (call regularly)
    void update_balance(double current_balance);
    void update_daily_pnl(double daily_pnl);
    void update_btc_volatility(double recent_move_percent);
    void record_error(const std::string& error_type);
    void record_success();

    // Evaluate and update mode (call from main loop)
    void evaluate();

    // Current state queries
    OperatingMode current_mode() const { return mode_.load(); }
    TradingRestrictions current_restrictions() const;
    bool can_open_position() const;
    bool can_place_order() const;
    double adjusted_max_size(double base_size) const;
    double adjusted_min_edge(double base_edge) const;

    // Manual controls
    bool set_mode(OperatingMode mode, const std::string& reason);
    bool upgrade_mode();  // Try to improve mode
    bool downgrade_mode(const std::string& reason);  // Force downgrade

    // Callbacks
    void set_mode_change_callback(ModeChangeCallback cb) { on_mode_change_ = std::move(cb); }

    // Event history
    std::vector<DegradationEvent> get_event_history() const;
    std::string status_summary() const;

private:
    std::shared_ptr<ConnectionHealthMonitor> health_monitor_;
    double starting_balance_;
    Config config_;

    std::atomic<OperatingMode> mode_{OperatingMode::NORMAL};
    mutable std::mutex mutex_;

    double current_balance_{0.0};
    double daily_pnl_{0.0};
    double btc_volatility_{0.0};

    std::vector<std::pair<WallClock, std::string>> recent_errors_;
    int consecutive_healthy_checks_{0};
    WallClock last_mode_change_;

    std::vector<DegradationEvent> event_history_;
    ModeChangeCallback on_mode_change_;

    // Internal evaluation
    OperatingMode determine_mode() const;
    std::vector<std::string> get_degradation_triggers() const;
    bool can_upgrade() const;
    void transition_mode(OperatingMode new_mode, const std::string& reason,
                        const std::vector<std::string>& triggers = {});
    void cleanup_old_errors();
    int error_count() const;
};

/**
 * RAII guard that checks restrictions before placing an order.
 */
class OrderRestrictionGuard {
public:
    OrderRestrictionGuard(const DegradationManager& manager)
        : allowed_(manager.can_place_order())
        , restrictions_(manager.current_restrictions())
    {}

    bool is_allowed() const { return allowed_; }
    const TradingRestrictions& restrictions() const { return restrictions_; }

private:
    bool allowed_;
    TradingRestrictions restrictions_;
};

} // namespace arb
