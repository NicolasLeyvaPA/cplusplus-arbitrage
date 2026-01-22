#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <optional>
#include "common/types.hpp"

namespace arb {

/**
 * Kill switch reasons - ordered by severity.
 */
enum class KillReason {
    MANUAL,              // Operator initiated
    DAILY_LOSS_LIMIT,    // Hit daily loss cap
    TOTAL_LOSS_LIMIT,    // Hit total loss cap (from starting balance)
    EXPOSURE_BREACH,     // Exceeded hard exposure limit
    CONNECTIVITY_LOSS,   // Lost connection to critical data source
    HIGH_SLIPPAGE,       // Excessive slippage detected
    RECONCILIATION_FAIL, // Failed to reconcile with exchange
    UNHEDGED_POSITION,   // Paired order left unhedged
    RATE_LIMIT_BREACH,   // Hitting rate limits too often
    SYSTEM_ERROR,        // Unexpected system error
    UNKNOWN
};

inline std::string kill_reason_to_string(KillReason r) {
    switch (r) {
        case KillReason::MANUAL: return "MANUAL";
        case KillReason::DAILY_LOSS_LIMIT: return "DAILY_LOSS_LIMIT";
        case KillReason::TOTAL_LOSS_LIMIT: return "TOTAL_LOSS_LIMIT";
        case KillReason::EXPOSURE_BREACH: return "EXPOSURE_BREACH";
        case KillReason::CONNECTIVITY_LOSS: return "CONNECTIVITY_LOSS";
        case KillReason::HIGH_SLIPPAGE: return "HIGH_SLIPPAGE";
        case KillReason::RECONCILIATION_FAIL: return "RECONCILIATION_FAIL";
        case KillReason::UNHEDGED_POSITION: return "UNHEDGED_POSITION";
        case KillReason::RATE_LIMIT_BREACH: return "RATE_LIMIT_BREACH";
        case KillReason::SYSTEM_ERROR: return "SYSTEM_ERROR";
        default: return "UNKNOWN";
    }
}

/**
 * Kill switch event for audit trail.
 */
struct KillEvent {
    WallClock timestamp;
    KillReason reason;
    std::string details;
    bool is_activation;  // true = activated, false = deactivated
};

/**
 * Enhanced kill switch with multiple trigger conditions and audit trail.
 *
 * DESIGN PRINCIPLES:
 * - Kill switch activation is IMMEDIATE and ATOMIC
 * - Deactivation requires explicit operator action
 * - All activations are logged for audit
 * - Hard limits cannot be bypassed by configuration
 */
class KillSwitch {
public:
    using Callback = std::function<void(KillReason, const std::string&)>;

    // Hard-coded absolute limits (cannot be changed via config)
    static constexpr double ABSOLUTE_MAX_LOSS_PERCENT = 0.25;     // Never lose more than 25% of starting
    static constexpr double ABSOLUTE_MAX_EXPOSURE = 10000.0;      // Never exceed $10k total exposure
    static constexpr int ABSOLUTE_MAX_OPEN_POSITIONS = 20;        // Never exceed 20 positions
    static constexpr int MAX_RATE_LIMIT_BREACHES = 5;             // Auto-kill after 5 rate limit hits

    struct Config {
        double daily_loss_limit{5.0};           // Soft daily loss limit
        double total_loss_limit_percent{0.10};  // Soft total loss limit (10% of starting)
        double max_exposure{100.0};             // Soft max exposure
        int max_connectivity_failures{10};       // Max connection failures before kill
        double high_slippage_bps{100.0};        // Slippage threshold
        int max_slippage_events{3};             // Max high-slippage events in window
        std::chrono::minutes slippage_window{5}; // Window for slippage events
    };

    explicit KillSwitch(double starting_balance, const Config& config = Config{});

    // Core state queries
    bool is_active() const { return active_.load(std::memory_order_acquire); }
    KillReason reason() const;
    std::string details() const;
    WallClock activation_time() const;

    // Activation (any component can call these)
    void activate(KillReason reason, const std::string& details = "");
    void activate_manual(const std::string& operator_note = "");

    // Deactivation (requires explicit action)
    bool deactivate(const std::string& operator_note);

    // Condition checks - call these regularly
    // Returns true if kill switch was just activated
    bool check_daily_loss(double current_daily_pnl);
    bool check_total_loss(double current_balance);
    bool check_exposure(double current_exposure);
    bool check_position_count(int open_positions);
    bool check_connectivity(int consecutive_failures);
    bool check_slippage(double slippage_bps);
    bool check_rate_limit_breach();

    // Register callback for activation events
    void set_callback(Callback cb) { callback_ = std::move(cb); }

    // Audit trail
    std::vector<KillEvent> get_event_history() const;
    void clear_history();

    // Hard limit checks (always enforced, cannot be disabled)
    static bool would_breach_absolute_limits(
        double current_balance,
        double starting_balance,
        double exposure,
        int positions
    );

private:
    std::atomic<bool> active_{false};
    double starting_balance_;
    Config config_;

    mutable std::mutex state_mutex_;
    KillReason current_reason_{KillReason::UNKNOWN};
    std::string current_details_;
    WallClock activation_time_;

    mutable std::mutex history_mutex_;
    std::vector<KillEvent> event_history_;

    // Slippage tracking
    mutable std::mutex slippage_mutex_;
    std::vector<std::pair<WallClock, double>> slippage_events_;

    // Rate limit tracking
    std::atomic<int> rate_limit_breaches_{0};

    Callback callback_;

    void record_event(KillReason reason, const std::string& details, bool is_activation);
    void invoke_callback(KillReason reason, const std::string& details);
    void cleanup_old_slippage_events();
};

} // namespace arb
