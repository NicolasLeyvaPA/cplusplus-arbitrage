#pragma once

#include "common/types.hpp"
#include "config/config.hpp"
#include "exchange/exchange_interface.hpp"
#include <map>
#include <mutex>
#include <atomic>

namespace arb {

class ExecutionEngine {
public:
    ExecutionEngine(ExchangeInterface& spot,
                    FuturesInterface& futures,
                    TradingMode mode,
                    HedgeMode hedge_mode = HedgeMode::USDC_COLLATERAL,
                    const FeeConfig& fees = FeeConfig{});

    struct ExecutionResult {
        bool success{false};
        OrderResponse spot_response;
        OrderResponse futures_response;
        std::string error;
    };

    ExecutionResult open_delta_neutral(const std::string& symbol,
                                       Size size,
                                       Price spot_price,
                                       Price futures_price);

    ExecutionResult close_delta_neutral(const std::string& symbol,
                                        const DeltaNeutralPosition& position);

    OrderResponse execute_order(const OrderRequest& req);

    TradingMode mode() const { return mode_; }
    HedgeMode hedge_mode() const { return hedge_mode_; }

private:
    OrderResponse paper_execute(const OrderRequest& req);
    OrderResponse dry_run_execute(const OrderRequest& req);

    ExchangeInterface& spot_;
    FuturesInterface& futures_;
    TradingMode mode_;
    HedgeMode hedge_mode_;
    FeeConfig fees_;

    struct PaperState {
        double usdt_balance{10000.0};
        std::map<std::string, double> spot_holdings;
        std::map<std::string, double> futures_positions;
    };
    PaperState paper_state_;
    mutable std::mutex paper_mutex_;
    std::atomic<uint64_t> next_order_id_{1};
};

} // namespace arb
