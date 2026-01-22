#pragma once

#include <atomic>
#include <mutex>
#include <chrono>
#include <deque>
#include "common/types.hpp"
#include "config/config.hpp"

namespace arb {

/**
 * Risk manager enforces all trading constraints.
 * Thread-safe for concurrent access from multiple components.
 */
class RiskManager {
public:
    explicit RiskManager(const RiskConfig& config, double starting_balance);

    // Pre-trade checks
    struct CheckResult {
        bool allowed{false};
        std::string reason;
    };

    CheckResult check_order(const Signal& signal, Notional notional) const;
    CheckResult check_position_limit(const std::string& market_id) const;
    CheckResult check_daily_loss() const;

    // Position updates
    void record_fill(const Fill& fill);
    void record_pnl(double realized_pnl);

    // Exposure queries
    double current_exposure() const;
    double exposure_for_market(const std::string& market_id) const;
    int open_position_count() const;

    // Daily PnL
    double daily_pnl() const { return daily_pnl_.load(); }
    double daily_loss_remaining() const;

    // Kill switch
    bool is_kill_switch_active() const { return kill_switch_.load(); }
    void activate_kill_switch(const std::string& reason);
    void deactivate_kill_switch();
    std::string kill_switch_reason() const;

    // Connectivity/slippage monitoring
    void record_slippage(double slippage_bps);
    void record_connectivity_issue();
    bool should_halt_trading() const;

    // Rate limiting
    bool can_place_order();
    void record_order_placed();

    // Reset daily counters (call at start of trading day)
    void reset_daily_counters();

    // Configuration access
    const RiskConfig& config() const { return config_; }

    // Balance management
    double available_balance() const;
    void update_balance(double new_balance);

private:
    RiskConfig config_;
    double starting_balance_;
    std::atomic<double> current_balance_;
    std::atomic<double> daily_pnl_{0.0};

    // Position tracking by market
    mutable std::mutex position_mutex_;
    std::map<std::string, double> market_exposure_;
    int open_positions_{0};

    // Kill switch
    std::atomic<bool> kill_switch_{false};
    std::string kill_switch_reason_;
    mutable std::mutex kill_switch_mutex_;

    // Slippage tracking
    mutable std::mutex slippage_mutex_;
    std::deque<std::pair<Timestamp, double>> recent_slippage_;
    static constexpr size_t MAX_SLIPPAGE_SAMPLES = 50;

    // Connectivity tracking
    std::atomic<int> connectivity_issues_{0};
    Timestamp last_connectivity_issue_{};  // Value-initialized to epoch

    // Rate limiting
    mutable std::mutex rate_limit_mutex_;
    std::deque<Timestamp> order_timestamps_;
};

} // namespace arb
