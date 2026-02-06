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

} // namespace arb
