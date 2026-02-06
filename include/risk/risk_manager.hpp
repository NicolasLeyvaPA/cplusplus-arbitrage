#pragma once

#include "common/types.hpp"
#include "config/config.hpp"
#include "position/position_manager.hpp"
#include <atomic>
#include <mutex>
#include <string>

namespace arb {

class RiskManager {
public:
    explicit RiskManager(const RiskConfig& config);

    struct RiskCheck {
        bool allowed{false};
        std::string reason;
    };

    RiskCheck check_new_position(const std::string& symbol,
                                  Notional exposure,
                                  const PositionManager& positions);
    RiskCheck check_daily_loss(double daily_pnl);
    RiskCheck check_drawdown(double current_equity, double high_water_mark);
    RiskCheck check_funding_rate(double funding_rate);
    RiskCheck check_basis_risk(double basis_bps);

    bool is_kill_switch_active() const { return kill_switch_.load(); }
    void activate_kill_switch(const std::string& reason);
    void deactivate_kill_switch();
    std::string kill_reason() const;

    void record_daily_pnl(double pnl);
    void reset_daily();

    const RiskConfig& config() const { return config_; }

private:
    RiskConfig config_;
    std::atomic<bool> kill_switch_{false};
    mutable std::mutex mutex_;
    std::string kill_reason_;
    double daily_pnl_{0};
};

} // namespace arb
