#pragma once

#include "common/types.hpp"
#include "execution/execution_engine.hpp"

namespace arb {

class HedgeManager {
public:
    HedgeManager(ExecutionEngine& execution, double max_imbalance_pct = 2.0);

    struct RebalanceResult {
        bool needed{false};
        bool executed{false};
        std::string symbol;
        double delta_before{0};
        double delta_after{0};
        std::string reason;
    };

    RebalanceResult check_and_rebalance(
        const std::string& symbol,
        const DeltaNeutralPosition& position,
        const Ticker& spot_ticker,
        const Ticker& futures_ticker);

    bool emergency_close(const std::string& symbol,
                          const DeltaNeutralPosition& position);

private:
    ExecutionEngine& execution_;
    double max_imbalance_pct_;
};

} // namespace arb
