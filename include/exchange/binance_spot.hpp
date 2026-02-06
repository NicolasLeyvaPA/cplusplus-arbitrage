#pragma once

#include "exchange/exchange_interface.hpp"
#include "exchange/binance_common.hpp"
#include "config/config.hpp"
#include <atomic>
#include <memory>

namespace arb {

class BinanceSpot : public ExchangeInterface {
public:
    explicit BinanceSpot(const ExchangeConfig& config);

    // ExchangeInterface
    Ticker get_ticker(const std::string& symbol) override;
    AccountBalance get_balance() override;
    OrderResponse place_order(const OrderRequest& req) override;
    bool cancel_order(const std::string& symbol, const std::string& order_id) override;
    std::vector<OrderResponse> get_open_orders(const std::string& symbol) override;
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;
    ConnectionStatus status() const override;

private:
    std::unique_ptr<BinanceHttpClient> http_;
    std::atomic<ConnectionStatus> status_{ConnectionStatus::DISCONNECTED};
};

} // namespace arb
