#pragma once

#include "common/types.hpp"
#include "config/config.hpp"
#include "strategy/funding_monitor.hpp"
#include "strategy/basis_calculator.hpp"
#include <map>
#include <vector>
#include <optional>

namespace arb {

/**
 * Core delta-neutral strategy engine.
 *
 * Algorithm:
 * 1. Monitor funding rates across perpetual futures
 * 2. When funding is significantly positive (longs pay shorts):
 *    - Buy spot asset
 *    - Short equivalent perpetual future
 *    - Collect funding payments every 8 hours
 * 3. When funding turns negative or basis collapses, unwind both legs
 */
class DeltaNeutralEngine {
public:
    DeltaNeutralEngine(FundingMonitor& funding,
                       BasisCalculator& basis,
                       const StrategyConfig& strategy_config,
                       const RiskConfig& risk_config);

    /**
     * Main evaluation: produces signals based on current market state.
     * Called periodically from the main loop.
     */
    std::vector<DeltaNeutralSignal> evaluate(
        const std::map<std::string, DeltaNeutralPosition>& positions,
        double available_capital);

private:
    std::optional<DeltaNeutralSignal> evaluate_entry(
        const std::string& symbol,
        double available_capital);

    std::optional<DeltaNeutralSignal> evaluate_exit(
        const std::string& symbol,
        const DeltaNeutralPosition& position);

    std::optional<DeltaNeutralSignal> evaluate_rebalance(
        const std::string& symbol,
        const DeltaNeutralPosition& position);

    Size calculate_position_size(const std::string& symbol,
                                  double capital,
                                  Price price);

    FundingMonitor& funding_;
    BasisCalculator& basis_;
    StrategyConfig strategy_config_;
    RiskConfig risk_config_;
};

} // namespace arb
