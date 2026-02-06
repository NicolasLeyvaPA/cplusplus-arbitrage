#include "execution/hedge_manager.hpp"
#include <spdlog/spdlog.h>
#include <cmath>

namespace arb {

HedgeManager::HedgeManager(ExecutionEngine& execution, double max_imbalance_pct)
    : execution_(execution), max_imbalance_pct_(max_imbalance_pct) {}

HedgeManager::RebalanceResult HedgeManager::check_and_rebalance(
    const std::string& symbol,
    const DeltaNeutralPosition& position,
    const Ticker& spot_ticker,
    const Ticker& futures_ticker)
{
    RebalanceResult result;
    result.symbol = symbol;
    result.delta_before = position.net_delta();

    if (position.is_balanced(max_imbalance_pct_)) {
        result.needed = false;
        return result;
    }

    result.needed = true;
    double delta = position.net_delta();

    spdlog::info("Rebalancing {}: delta={:.6f}, spot={:.6f}, futures={:.6f}",
                 symbol, delta, position.spot_size, position.futures_size);

    if (delta > 0) {
        // Too much long exposure - sell more futures
        OrderRequest req;
        req.symbol = symbol;
        req.market = MarketType::FUTURES;
        req.side = Side::SELL;
        req.type = OrderType::MARKET;
        req.quantity = std::abs(delta);
        req.price = futures_ticker.bid;

        auto resp = execution_.execute_order(req);
        result.executed = resp.success();
        if (result.executed) {
            result.delta_after = delta - resp.executed_qty;
        }
    } else {
        // Too much short exposure - buy more futures
        OrderRequest req;
        req.symbol = symbol;
        req.market = MarketType::FUTURES;
        req.side = Side::BUY;
        req.type = OrderType::MARKET;
        req.quantity = std::abs(delta);
        req.price = futures_ticker.ask;

        auto resp = execution_.execute_order(req);
        result.executed = resp.success();
        if (result.executed) {
            result.delta_after = delta + resp.executed_qty;
        }
    }

    result.reason = fmt::format("Delta rebalanced from {:.6f} to {:.6f}",
                                 result.delta_before, result.delta_after);
    spdlog::info("{}", result.reason);
    return result;
}

bool HedgeManager::emergency_close(const std::string& symbol,
                                    const DeltaNeutralPosition& position) {
    spdlog::warn("EMERGENCY CLOSE: {}", symbol);
    auto result = execution_.close_delta_neutral(symbol, position);
    return result.success;
}

} // namespace arb
