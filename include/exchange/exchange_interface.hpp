#pragma once

#include "common/types.hpp"
#include <string>
#include <vector>

namespace arb {

class ExchangeInterface {
public:
    virtual ~ExchangeInterface() = default;

    virtual Ticker get_ticker(const std::string& symbol) = 0;
    virtual AccountBalance get_balance() = 0;

    virtual OrderResponse place_order(const OrderRequest& req) = 0;
    virtual bool cancel_order(const std::string& symbol, const std::string& order_id) = 0;
    virtual std::vector<OrderResponse> get_open_orders(const std::string& symbol) = 0;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    virtual ConnectionStatus status() const = 0;
};

/**
 * Extended interface for exchanges that support perpetual futures.
 * Adds funding rate queries, leverage control, and position management.
 */
class FuturesInterface : public ExchangeInterface {
public:
    virtual ~FuturesInterface() = default;

    virtual FundingInfo get_funding_info(const std::string& symbol) = 0;
    virtual std::vector<FundingInfo> get_all_funding_rates() = 0;
    virtual std::vector<FundingInfo> get_funding_history(const std::string& symbol, int limit = 100) = 0;
    virtual bool set_leverage(const std::string& symbol, int leverage) = 0;

    struct FuturesPosition {
        std::string symbol;
        double position_amt{0};
        double entry_price{0};
        double mark_price{0};
        double unrealized_profit{0};
        double leverage{1};
        std::string margin_type;
    };
    virtual std::vector<FuturesPosition> get_positions() = 0;
};

/**
 * No-op exchange for USDC_COLLATERAL mode (no spot leg needed).
 */
class NullExchange : public ExchangeInterface {
public:
    Ticker get_ticker(const std::string&) override { return {}; }
    AccountBalance get_balance() override { return {}; }
    OrderResponse place_order(const OrderRequest&) override {
        OrderResponse r;
        r.error_message = "NullExchange: no-op";
        r.status = OrderStatus::REJECTED;
        return r;
    }
    bool cancel_order(const std::string&, const std::string&) override { return false; }
    std::vector<OrderResponse> get_open_orders(const std::string&) override { return {}; }
    bool connect() override { return true; }
    void disconnect() override {}
    bool is_connected() const override { return true; }
    ConnectionStatus status() const override { return ConnectionStatus::CONNECTED; }
};

} // namespace arb
