#include "execution/execution_engine.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>

namespace arb {

ExecutionEngine::ExecutionEngine(ExchangeInterface& spot, FuturesInterface& futures,
                                 TradingMode mode, HedgeMode hedge_mode, const FeeConfig& fees)
    : spot_(spot), futures_(futures), mode_(mode), hedge_mode_(hedge_mode), fees_(fees)
{
    spdlog::info("ExecutionEngine initialized: mode={}, hedge={}", arb::to_string(mode), arb::to_string(hedge_mode));
}

OrderResponse ExecutionEngine::execute_order(const OrderRequest& req) {
    switch (mode_) {
        case TradingMode::DRY_RUN:
            return dry_run_execute(req);
        case TradingMode::PAPER:
            return paper_execute(req);
        case TradingMode::LIVE:
            if (req.market == MarketType::SPOT) {
                return spot_.place_order(req);
            } else {
                return futures_.place_order(req);
            }
    }
    OrderResponse resp;
    resp.error_message = "Unknown trading mode";
    return resp;
}

OrderResponse ExecutionEngine::dry_run_execute(const OrderRequest& req) {
    OrderResponse resp;
    resp.order_id = "DRY-" + std::to_string(next_order_id_++);
    resp.client_order_id = req.client_order_id;
    resp.symbol = req.symbol;
    resp.status = OrderStatus::NEW;
    resp.side = req.side;
    resp.price = req.price;
    resp.original_qty = req.quantity;
    resp.executed_qty = 0;
    resp.timestamp_ms = static_cast<uint64_t>(now_ms());

    spdlog::info("[DRY RUN] {} {} {} {:.6f} @ {:.2f}",
                 arb::to_string(req.market), arb::to_string(req.side),
                 req.symbol, req.quantity, req.price);
    return resp;
}

OrderResponse ExecutionEngine::paper_execute(const OrderRequest& req) {
    std::lock_guard<std::mutex> lock(paper_mutex_);

    OrderResponse resp;
    resp.order_id = "PAPER-" + std::to_string(next_order_id_++);
    resp.client_order_id = req.client_order_id;
    resp.symbol = req.symbol;
    resp.side = req.side;
    resp.price = req.price > 0 ? req.price : 0;
    resp.original_qty = req.quantity;
    resp.timestamp_ms = static_cast<uint64_t>(now_ms());

    double notional = req.quantity * req.price;
    // Use fee config instead of hardcoded 0.04%
    double fee_rate = (req.market == MarketType::FUTURES) ? fees_.perp_taker_fee_pct : fees_.spot_taker_fee_pct;
    double fee = notional * fee_rate;

    if (req.market == MarketType::SPOT) {
        if (req.side == Side::BUY) {
            if (paper_state_.usdt_balance >= notional + fee) {
                paper_state_.usdt_balance -= (notional + fee);
                paper_state_.spot_holdings[req.symbol] += req.quantity;
                resp.status = OrderStatus::FILLED;
                resp.executed_qty = req.quantity;
                resp.cumulative_quote_qty = notional;
            } else {
                resp.status = OrderStatus::REJECTED;
                resp.error_message = "Insufficient paper balance";
            }
        } else {
            if (paper_state_.spot_holdings[req.symbol] >= req.quantity) {
                paper_state_.spot_holdings[req.symbol] -= req.quantity;
                paper_state_.usdt_balance += (notional - fee);
                resp.status = OrderStatus::FILLED;
                resp.executed_qty = req.quantity;
                resp.cumulative_quote_qty = notional;
            } else {
                resp.status = OrderStatus::REJECTED;
                resp.error_message = "Insufficient spot holdings";
            }
        }
    } else {
        // Futures: margin-based
        if (req.side == Side::SELL) {
            paper_state_.futures_positions[req.symbol] -= req.quantity;
        } else {
            paper_state_.futures_positions[req.symbol] += req.quantity;
        }
        paper_state_.usdt_balance -= fee;
        resp.status = OrderStatus::FILLED;
        resp.executed_qty = req.quantity;
        resp.cumulative_quote_qty = notional;
    }

    spdlog::info("[PAPER] {} {} {} {:.6f} @ {:.2f} -> {} (fee: ${:.4f})",
                 arb::to_string(req.market), arb::to_string(req.side),
                 req.symbol, req.quantity, req.price,
                 arb::to_string(resp.status), fee);
    return resp;
}

ExecutionEngine::ExecutionResult ExecutionEngine::open_delta_neutral(
    const std::string& symbol, Size size, Price spot_price, Price futures_price)
{
    ExecutionResult result;

    spdlog::info("Opening delta-neutral: {} size={:.6f} spot={:.2f} futures={:.2f} hedge={}",
                 symbol, size, spot_price, futures_price, arb::to_string(hedge_mode_));

    // Leg 1: Buy spot (only in FULL_HEDGE mode)
    if (hedge_mode_ == HedgeMode::FULL_HEDGE) {
        OrderRequest spot_req;
        spot_req.symbol = symbol;
        spot_req.market = MarketType::SPOT;
        spot_req.side = Side::BUY;
        spot_req.type = OrderType::MARKET;
        spot_req.quantity = size;
        spot_req.price = spot_price;

        result.spot_response = execute_order(spot_req);
        if (!result.spot_response.success()) {
            result.error = "Spot leg failed: " + result.spot_response.error_message;
            spdlog::error("{}", result.error);
            return result;
        }
    } else {
        // USDC_COLLATERAL: no spot leg, just mark as filled
        result.spot_response.status = OrderStatus::FILLED;
        result.spot_response.executed_qty = 0;
        result.spot_response.symbol = symbol;
    }

    // Leg 2: Short futures
    OrderRequest futures_req;
    futures_req.symbol = symbol;
    futures_req.market = MarketType::FUTURES;
    futures_req.side = Side::SELL;
    futures_req.type = OrderType::MARKET;
    futures_req.quantity = size;
    futures_req.price = futures_price;

    result.futures_response = execute_order(futures_req);
    if (!result.futures_response.success()) {
        result.error = "Futures leg failed: " + result.futures_response.error_message;
        spdlog::error("{}. {} unwind needed!", result.error,
                       hedge_mode_ == HedgeMode::FULL_HEDGE ? "Spot leg filled -" : "No");
        return result;
    }

    result.success = true;
    spdlog::info("Delta-neutral opened: {} {:.6f} units ({})", symbol, size, arb::to_string(hedge_mode_));
    return result;
}

ExecutionEngine::ExecutionResult ExecutionEngine::close_delta_neutral(
    const std::string& symbol, const DeltaNeutralPosition& position)
{
    ExecutionResult result;

    spdlog::info("Closing delta-neutral: {} spot={:.6f} futures={:.6f}",
                 symbol, position.spot_size, position.futures_size);

    // Leg 1: Close futures short (buy to cover)
    if (position.futures_size != 0) {
        OrderRequest futures_req;
        futures_req.symbol = symbol;
        futures_req.market = MarketType::FUTURES;
        futures_req.side = Side::BUY;
        futures_req.type = OrderType::MARKET;
        futures_req.quantity = std::abs(position.futures_size);
        futures_req.price = position.futures_avg_entry;
        futures_req.reduce_only = true;

        result.futures_response = execute_order(futures_req);
        if (!result.futures_response.success()) {
            result.error = "Close futures failed: " + result.futures_response.error_message;
            spdlog::error("{}", result.error);
        }
    }

    // Leg 2: Sell spot (only in FULL_HEDGE mode)
    if (hedge_mode_ == HedgeMode::FULL_HEDGE && position.spot_size > 0) {
        OrderRequest spot_req;
        spot_req.symbol = symbol;
        spot_req.market = MarketType::SPOT;
        spot_req.side = Side::SELL;
        spot_req.type = OrderType::MARKET;
        spot_req.quantity = position.spot_size;
        spot_req.price = position.spot_avg_entry;

        result.spot_response = execute_order(spot_req);
        if (!result.spot_response.success()) {
            result.error = "Close spot failed: " + result.spot_response.error_message;
            spdlog::error("{}", result.error);
        }
    } else {
        result.spot_response.status = OrderStatus::FILLED;
        result.spot_response.executed_qty = 0;
    }

    bool futures_ok = result.futures_response.success() || position.futures_size == 0;
    bool spot_ok = result.spot_response.success() || hedge_mode_ == HedgeMode::USDC_COLLATERAL;
    result.success = futures_ok && spot_ok;

    if (result.success) {
        spdlog::info("Delta-neutral closed: {}", symbol);
    }
    return result;
}

} // namespace arb
