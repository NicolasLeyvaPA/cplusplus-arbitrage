#include "exchange/binance_spot.hpp"
#include <spdlog/spdlog.h>

namespace arb {

BinanceSpot::BinanceSpot(const ExchangeConfig& config) {
    std::string api_key = Config::get_env(config.api_key_env);
    std::string api_secret = Config::get_env(config.api_secret_env);
    http_ = std::make_unique<BinanceHttpClient>(config.spot_rest_url, api_key, api_secret);
}

bool BinanceSpot::connect() {
    try {
        http_->public_get("/api/v3/ping");
        status_ = ConnectionStatus::CONNECTED;
        spdlog::info("Binance Spot connected");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Binance Spot connection failed: {}", e.what());
        status_ = ConnectionStatus::ERROR;
        return false;
    }
}

void BinanceSpot::disconnect() { status_ = ConnectionStatus::DISCONNECTED; }
bool BinanceSpot::is_connected() const { return status_ == ConnectionStatus::CONNECTED; }
ConnectionStatus BinanceSpot::status() const { return status_.load(); }

Ticker BinanceSpot::get_ticker(const std::string& symbol) {
    auto j = http_->public_get("/api/v3/ticker/bookTicker", {{"symbol", symbol}});
    Ticker t;
    t.symbol = symbol;
    t.bid = std::stod(j["bidPrice"].get<std::string>());
    t.ask = std::stod(j["askPrice"].get<std::string>());
    t.bid_qty = std::stod(j["bidQty"].get<std::string>());
    t.ask_qty = std::stod(j["askQty"].get<std::string>());
    t.timestamp_ms = static_cast<uint64_t>(now_ms());
    return t;
}

AccountBalance BinanceSpot::get_balance() {
    auto j = http_->signed_get("/api/v3/account");
    AccountBalance bal;
    if (j.contains("balances")) {
        for (const auto& b : j["balances"]) {
            std::string asset = b["asset"];
            double free = std::stod(b["free"].get<std::string>());
            double locked = std::stod(b["locked"].get<std::string>());
            double total = free + locked;
            if (total > 0) bal.asset_balances[asset] = total;
            if (asset == "USDT") {
                bal.available_balance = free;
                bal.total_balance = total;
            }
        }
    }
    return bal;
}

OrderResponse BinanceSpot::place_order(const OrderRequest& req) {
    std::map<std::string, std::string> params;
    params["symbol"] = req.symbol;
    params["side"] = arb::to_string(req.side);
    params["quantity"] = std::to_string(req.quantity);

    switch (req.type) {
        case OrderType::MARKET: params["type"] = "MARKET"; break;
        case OrderType::LIMIT:
            params["type"] = "LIMIT";
            params["timeInForce"] = "GTC";
            params["price"] = std::to_string(req.price);
            break;
        case OrderType::LIMIT_MAKER:
            params["type"] = "LIMIT_MAKER";
            params["price"] = std::to_string(req.price);
            break;
    }
    if (!req.client_order_id.empty()) params["newClientOrderId"] = req.client_order_id;

    OrderResponse resp;
    try {
        auto j = http_->signed_post("/api/v3/order", params);
        resp.order_id = std::to_string(j["orderId"].get<int64_t>());
        resp.client_order_id = j.value("clientOrderId", "");
        resp.symbol = j["symbol"];
        resp.status = order_status_from_str(j["status"]);
        resp.side = side_from_string(j["side"]);
        resp.price = std::stod(j.value("price", "0"));
        resp.original_qty = std::stod(j["origQty"].get<std::string>());
        resp.executed_qty = std::stod(j["executedQty"].get<std::string>());
        resp.cumulative_quote_qty = std::stod(j.value("cummulativeQuoteQty", "0"));
        resp.timestamp_ms = j.value("transactTime", static_cast<uint64_t>(now_ms()));
        spdlog::info("Spot order: {} {} {} @ {}", arb::to_string(resp.side), resp.symbol, resp.original_qty, resp.price);
    } catch (const std::exception& e) {
        resp.error_message = e.what();
        spdlog::error("Spot order failed: {}", e.what());
    }
    return resp;
}

bool BinanceSpot::cancel_order(const std::string& symbol, const std::string& order_id) {
    try {
        http_->signed_delete("/api/v3/order", {{"symbol", symbol}, {"orderId", order_id}});
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Cancel spot order failed: {}", e.what());
        return false;
    }
}

std::vector<OrderResponse> BinanceSpot::get_open_orders(const std::string& symbol) {
    std::map<std::string, std::string> params;
    if (!symbol.empty()) params["symbol"] = symbol;

    std::vector<OrderResponse> orders;
    try {
        auto j = http_->signed_get("/api/v3/openOrders", params);
        for (const auto& o : j) {
            OrderResponse resp;
            resp.order_id = std::to_string(o["orderId"].get<int64_t>());
            resp.symbol = o["symbol"];
            resp.status = order_status_from_str(o["status"]);
            resp.side = side_from_string(o["side"]);
            resp.price = std::stod(o["price"].get<std::string>());
            resp.original_qty = std::stod(o["origQty"].get<std::string>());
            resp.executed_qty = std::stod(o["executedQty"].get<std::string>());
            orders.push_back(resp);
        }
    } catch (const std::exception& e) {
        spdlog::error("Get open orders failed: {}", e.what());
    }
    return orders;
}

} // namespace arb
