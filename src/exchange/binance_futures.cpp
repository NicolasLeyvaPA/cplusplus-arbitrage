#include "exchange/binance_futures.hpp"
#include <spdlog/spdlog.h>

namespace arb {

BinanceFutures::BinanceFutures(const ExchangeConfig& config) {
    std::string api_key = Config::get_env(config.api_key_env);
    std::string api_secret = Config::get_env(config.api_secret_env);
    http_ = std::make_unique<BinanceHttpClient>(config.futures_rest_url, api_key, api_secret);
}

bool BinanceFutures::connect() {
    try {
        http_->public_get("/fapi/v1/ping");
        status_ = ConnectionStatus::CONNECTED;
        spdlog::info("Binance Futures connected");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Binance Futures connection failed: {}", e.what());
        status_ = ConnectionStatus::ERROR;
        return false;
    }
}

void BinanceFutures::disconnect() { status_ = ConnectionStatus::DISCONNECTED; }
bool BinanceFutures::is_connected() const { return status_ == ConnectionStatus::CONNECTED; }
ConnectionStatus BinanceFutures::status() const { return status_.load(); }

Ticker BinanceFutures::get_ticker(const std::string& symbol) {
    auto j = http_->public_get("/fapi/v1/ticker/bookTicker", {{"symbol", symbol}});
    Ticker t;
    t.symbol = symbol;
    t.bid = std::stod(j["bidPrice"].get<std::string>());
    t.ask = std::stod(j["askPrice"].get<std::string>());
    t.bid_qty = std::stod(j["bidQty"].get<std::string>());
    t.ask_qty = std::stod(j["askQty"].get<std::string>());
    t.timestamp_ms = j.value("time", static_cast<uint64_t>(now_ms()));
    return t;
}

AccountBalance BinanceFutures::get_balance() {
    auto j = http_->signed_get("/fapi/v2/balance");
    AccountBalance bal;
    for (const auto& b : j) {
        std::string asset = b["asset"];
        double balance_val = std::stod(b["balance"].get<std::string>());
        double available = std::stod(b["availableBalance"].get<std::string>());
        if (balance_val > 0) bal.asset_balances[asset] = balance_val;
        if (asset == "USDT") {
            bal.total_balance = balance_val;
            bal.available_balance = available;
        }
    }
    return bal;
}

OrderResponse BinanceFutures::place_order(const OrderRequest& req) {
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
            params["type"] = "LIMIT";
            params["timeInForce"] = "GTX";
            params["price"] = std::to_string(req.price);
            break;
    }
    if (req.reduce_only) params["reduceOnly"] = "true";
    if (!req.client_order_id.empty()) params["newClientOrderId"] = req.client_order_id;

    OrderResponse resp;
    try {
        auto j = http_->signed_post("/fapi/v1/order", params);
        resp.order_id = std::to_string(j["orderId"].get<int64_t>());
        resp.client_order_id = j.value("clientOrderId", "");
        resp.symbol = j["symbol"];
        resp.status = order_status_from_str(j["status"]);
        resp.side = side_from_string(j["side"]);
        resp.price = std::stod(j.value("price", "0"));
        resp.original_qty = std::stod(j["origQty"].get<std::string>());
        resp.executed_qty = std::stod(j["executedQty"].get<std::string>());
        resp.cumulative_quote_qty = std::stod(j.value("cumQuote", "0"));
        resp.timestamp_ms = j.value("updateTime", static_cast<uint64_t>(now_ms()));
        spdlog::info("Futures order: {} {} {} @ {}", arb::to_string(resp.side), resp.symbol, resp.original_qty, resp.price);
    } catch (const std::exception& e) {
        resp.error_message = e.what();
        spdlog::error("Futures order failed: {}", e.what());
    }
    return resp;
}

bool BinanceFutures::cancel_order(const std::string& symbol, const std::string& order_id) {
    try {
        http_->signed_delete("/fapi/v1/order", {{"symbol", symbol}, {"orderId", order_id}});
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Cancel futures order failed: {}", e.what());
        return false;
    }
}

std::vector<OrderResponse> BinanceFutures::get_open_orders(const std::string& symbol) {
    std::map<std::string, std::string> params;
    if (!symbol.empty()) params["symbol"] = symbol;

    std::vector<OrderResponse> orders;
    try {
        auto j = http_->signed_get("/fapi/v1/openOrders", params);
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
        spdlog::error("Get open futures orders failed: {}", e.what());
    }
    return orders;
}

FundingInfo BinanceFutures::get_funding_info(const std::string& symbol) {
    auto j = http_->public_get("/fapi/v1/premiumIndex", {{"symbol", symbol}});
    FundingInfo info;
    info.symbol = j["symbol"];
    info.current_rate = std::stod(j["lastFundingRate"].get<std::string>());
    info.mark_price = std::stod(j["markPrice"].get<std::string>());
    info.index_price = std::stod(j["indexPrice"].get<std::string>());
    info.next_funding_time_ms = j["nextFundingTime"].get<uint64_t>();
    info.timestamp_ms = j.value("time", static_cast<uint64_t>(now_ms()));
    info.predicted_rate = (info.mark_price - info.index_price) / info.index_price;
    return info;
}

std::vector<FundingInfo> BinanceFutures::get_all_funding_rates() {
    auto j = http_->public_get("/fapi/v1/premiumIndex");
    std::vector<FundingInfo> rates;
    for (const auto& item : j) {
        FundingInfo info;
        info.symbol = item["symbol"];
        info.current_rate = std::stod(item["lastFundingRate"].get<std::string>());
        info.mark_price = std::stod(item["markPrice"].get<std::string>());
        info.index_price = std::stod(item["indexPrice"].get<std::string>());
        info.next_funding_time_ms = item["nextFundingTime"].get<uint64_t>();
        info.timestamp_ms = item.value("time", static_cast<uint64_t>(now_ms()));
        rates.push_back(info);
    }
    return rates;
}

std::vector<FundingInfo> BinanceFutures::get_funding_history(const std::string& symbol, int limit) {
    auto j = http_->public_get("/fapi/v1/fundingRate", {{"symbol", symbol}, {"limit", std::to_string(limit)}});
    std::vector<FundingInfo> history;
    for (const auto& item : j) {
        FundingInfo info;
        info.symbol = item["symbol"];
        info.current_rate = std::stod(item["fundingRate"].get<std::string>());
        info.timestamp_ms = item["fundingTime"].get<uint64_t>();
        history.push_back(info);
    }
    return history;
}

bool BinanceFutures::set_leverage(const std::string& symbol, int leverage) {
    try {
        http_->signed_post("/fapi/v1/leverage", {{"symbol", symbol}, {"leverage", std::to_string(leverage)}});
        spdlog::info("Set leverage for {} to {}x", symbol, leverage);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Set leverage failed: {}", e.what());
        return false;
    }
}

bool BinanceFutures::set_margin_type(const std::string& symbol, const std::string& type) {
    try {
        http_->signed_post("/fapi/v1/marginType", {{"symbol", symbol}, {"marginType", type}});
        spdlog::info("Set margin type for {} to {}", symbol, type);
        return true;
    } catch (const std::exception& e) {
        if (std::string(e.what()).find("-4046") != std::string::npos) return true;
        spdlog::error("Set margin type failed: {}", e.what());
        return false;
    }
}

std::vector<FuturesInterface::FuturesPosition> BinanceFutures::get_positions() {
    auto j = http_->signed_get("/fapi/v2/positionRisk");
    std::vector<FuturesPosition> positions;
    for (const auto& p : j) {
        double amt = std::stod(p["positionAmt"].get<std::string>());
        if (amt == 0) continue;
        FuturesPosition pos;
        pos.symbol = p["symbol"];
        pos.position_amt = amt;
        pos.entry_price = std::stod(p["entryPrice"].get<std::string>());
        pos.mark_price = std::stod(p["markPrice"].get<std::string>());
        pos.unrealized_profit = std::stod(p["unRealizedProfit"].get<std::string>());
        pos.leverage = std::stod(p["leverage"].get<std::string>());
        pos.margin_type = p["marginType"];
        positions.push_back(pos);
    }
    return positions;
}

} // namespace arb
