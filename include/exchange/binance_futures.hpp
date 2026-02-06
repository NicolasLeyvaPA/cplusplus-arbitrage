#pragma once

#include "exchange/exchange_interface.hpp"
#include "exchange/binance_common.hpp"
#include "config/config.hpp"
#include <atomic>
#include <memory>

namespace arb {

class BinanceFutures : public ExchangeInterface {
public:
    explicit BinanceFutures(const ExchangeConfig& config);

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

    // Futures-specific
    FundingInfo get_funding_info(const std::string& symbol);
    std::vector<FundingInfo> get_all_funding_rates();
    std::vector<FundingInfo> get_funding_history(const std::string& symbol, int limit = 100);
    bool set_leverage(const std::string& symbol, int leverage);
    bool set_margin_type(const std::string& symbol, const std::string& type);

    struct FuturesPosition {
        std::string symbol;
        double position_amt{0};
        double entry_price{0};
        double mark_price{0};
        double unrealized_profit{0};
        double leverage{1};
        std::string margin_type;
    };
    std::vector<FuturesPosition> get_positions();

private:
    std::unique_ptr<BinanceHttpClient> http_;
    std::atomic<ConnectionStatus> status_{ConnectionStatus::DISCONNECTED};
};

} // namespace arb
