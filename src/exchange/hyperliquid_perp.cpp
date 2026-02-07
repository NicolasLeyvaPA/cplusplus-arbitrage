#include "exchange/hyperliquid_perp.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace arb {

HyperliquidPerp::HyperliquidPerp(const ExchangeConfig& config)
    : config_(config)
{
    std::string private_key = Config::get_env(config.wallet_private_key_env);
    if (private_key.empty()) {
        spdlog::warn("Hyperliquid private key not set (env: {})", config.wallet_private_key_env);
    }

    signer_ = std::make_unique<HyperliquidSigner>(private_key, config.testnet);

    std::string user_addr = config.hl_user_address;
    if (user_addr.empty() && signer_->is_valid()) {
        user_addr = signer_->address();
    }

    http_ = std::make_unique<HyperliquidHttpClient>(config.hl_rest_url, *signer_, user_addr);
    spdlog::info("HyperliquidPerp initialized: url={}, address={}", config.hl_rest_url, user_addr);
}

// Symbol mapping: "BTCUSDT" -> "BTC", "ETHUSDT" -> "ETH"
std::string HyperliquidPerp::to_hl_coin(const std::string& internal_symbol) const {
    std::string coin = internal_symbol;
    // Strip common suffixes
    for (const auto& suffix : {"USDT", "USDC", "USD", "PERP"}) {
        size_t pos = coin.rfind(suffix);
        if (pos != std::string::npos && pos + std::strlen(suffix) == coin.size()) {
            coin = coin.substr(0, pos);
            break;
        }
    }
    return coin;
}

std::string HyperliquidPerp::from_hl_coin(const std::string& hl_coin) const {
    return hl_coin + "USDT";  // Internal format uses USDT suffix
}

int HyperliquidPerp::coin_to_asset_index(const std::string& coin) const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    auto it = asset_meta_.find(coin);
    if (it != asset_meta_.end()) return it->second.index;
    return -1;
}

void HyperliquidPerp::load_asset_metadata() {
    try {
        auto response = http_->info_request("meta");

        if (!response.contains("universe")) {
            spdlog::error("Hyperliquid meta response missing 'universe'");
            return;
        }

        std::lock_guard<std::mutex> lock(meta_mutex_);
        asset_meta_.clear();

        const auto& universe = response["universe"];
        for (size_t i = 0; i < universe.size(); i++) {
            AssetMeta meta;
            meta.index = static_cast<int>(i);
            meta.name = universe[i]["name"].get<std::string>();
            meta.sz_decimals = universe[i].value("szDecimals", 0);
            asset_meta_[meta.name] = meta;
        }
        meta_loaded_ = true;
        spdlog::info("Loaded {} asset metadata entries from Hyperliquid", asset_meta_.size());
    } catch (const std::exception& e) {
        spdlog::error("Failed to load Hyperliquid asset metadata: {}", e.what());
    }
}

bool HyperliquidPerp::connect() {
    status_ = ConnectionStatus::CONNECTING;
    try {
        // Test connectivity by fetching metadata
        load_asset_metadata();
        if (meta_loaded_) {
            status_ = ConnectionStatus::CONNECTED;
            spdlog::info("Connected to Hyperliquid");
            return true;
        }
    } catch (const std::exception& e) {
        spdlog::error("Hyperliquid connection failed: {}", e.what());
    }
    status_ = ConnectionStatus::ERROR;
    return false;
}

void HyperliquidPerp::disconnect() {
    status_ = ConnectionStatus::DISCONNECTED;
    spdlog::info("Disconnected from Hyperliquid");
}

bool HyperliquidPerp::is_connected() const {
    return status_ == ConnectionStatus::CONNECTED;
}

ConnectionStatus HyperliquidPerp::status() const {
    return status_.load();
}

Ticker HyperliquidPerp::get_ticker(const std::string& symbol) {
    std::string coin = to_hl_coin(symbol);
    auto response = http_->info_request("l2Book", {{"coin", coin}});

    Ticker ticker;
    ticker.symbol = symbol;
    ticker.timestamp_ms = static_cast<uint64_t>(now_ms());

    if (response.contains("levels") && response["levels"].is_array() && response["levels"].size() >= 2) {
        const auto& bids = response["levels"][0];  // bids
        const auto& asks = response["levels"][1];  // asks

        if (!bids.empty()) {
            ticker.bid = std::stod(bids[0]["px"].get<std::string>());
            ticker.bid_qty = std::stod(bids[0]["sz"].get<std::string>());
        }
        if (!asks.empty()) {
            ticker.ask = std::stod(asks[0]["px"].get<std::string>());
            ticker.ask_qty = std::stod(asks[0]["sz"].get<std::string>());
        }
    }

    return ticker;
}

AccountBalance HyperliquidPerp::get_balance() {
    auto response = http_->info_request("clearinghouseState");

    AccountBalance balance;
    if (response.contains("marginSummary")) {
        const auto& ms = response["marginSummary"];
        balance.total_balance = std::stod(ms.value("accountValue", "0"));
        balance.available_balance = std::stod(ms.value("totalRawUsd", "0"));
        balance.margin_balance = std::stod(ms.value("totalMarginUsed", "0"));
    }
    if (response.contains("crossMarginSummary")) {
        const auto& cms = response["crossMarginSummary"];
        balance.unrealized_pnl = std::stod(cms.value("totalNtlPos", "0"));
    }

    return balance;
}

OrderResponse HyperliquidPerp::place_order(const OrderRequest& req) {
    std::string coin = to_hl_coin(req.symbol);
    int asset_idx = coin_to_asset_index(coin);
    if (asset_idx < 0) {
        OrderResponse resp;
        resp.error_message = "Unknown asset: " + coin;
        resp.status = OrderStatus::REJECTED;
        return resp;
    }

    // Get precision for price formatting
    int sz_dec = 0;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        auto it = asset_meta_.find(coin);
        if (it != asset_meta_.end()) {
            sz_dec = it->second.sz_decimals;
        }
    }

    // Format size with correct decimals
    std::string sz_str = fmt::format("{:.{}f}", req.quantity, sz_dec);
    std::string px_str = fmt::format("{:.1f}", req.price);  // Prices: 1 decimal for most

    bool is_buy = (req.side == Side::BUY);

    // Build order action
    nlohmann::json order = {
        {"a", asset_idx},
        {"b", is_buy},
        {"p", px_str},
        {"s", sz_str},
        {"r", req.reduce_only},
        {"t", {{"limit", {{"tif", "Gtc"}}}}},
    };

    // Override TIF for market orders
    if (req.type == OrderType::MARKET) {
        // Use IOC with aggressive price for market-like execution
        order["t"] = {{"limit", {{"tif", "Ioc"}}}};
        // Adjust price to be more aggressive for market orders
        if (is_buy) {
            order["p"] = fmt::format("{:.1f}", req.price * 1.005);  // 0.5% slippage
        } else {
            order["p"] = fmt::format("{:.1f}", req.price * 0.995);
        }
    }

    nlohmann::json action = {
        {"type", "order"},
        {"orders", nlohmann::json::array({order})},
        {"grouping", "na"}
    };

    OrderResponse resp;
    resp.symbol = req.symbol;
    resp.side = req.side;
    resp.original_qty = req.quantity;
    resp.price = req.price;
    resp.timestamp_ms = static_cast<uint64_t>(now_ms());

    try {
        auto result = http_->exchange_request(action);

        if (result.contains("status") && result["status"] == "ok") {
            auto& statuses = result["response"]["data"]["statuses"];
            if (!statuses.empty()) {
                auto& s = statuses[0];
                if (s.contains("resting")) {
                    resp.order_id = std::to_string(s["resting"]["oid"].get<uint64_t>());
                    resp.status = OrderStatus::NEW;
                } else if (s.contains("filled")) {
                    resp.order_id = std::to_string(s["filled"]["oid"].get<uint64_t>());
                    resp.status = OrderStatus::FILLED;
                    resp.executed_qty = std::stod(s["filled"]["totalSz"].get<std::string>());
                    resp.price = std::stod(s["filled"]["avgPx"].get<std::string>());
                    resp.cumulative_quote_qty = resp.executed_qty * resp.price;
                } else if (s.contains("error")) {
                    resp.status = OrderStatus::REJECTED;
                    resp.error_message = s["error"].get<std::string>();
                }
            }
        } else {
            resp.status = OrderStatus::REJECTED;
            resp.error_message = result.dump().substr(0, 200);
        }
    } catch (const std::exception& e) {
        resp.status = OrderStatus::REJECTED;
        resp.error_message = e.what();
    }

    spdlog::info("[HL] {} {} {} {:.6f} @ {:.2f} -> {}",
                 arb::to_string(req.market), arb::to_string(req.side),
                 req.symbol, req.quantity, req.price, arb::to_string(resp.status));
    return resp;
}

bool HyperliquidPerp::cancel_order(const std::string& symbol, const std::string& order_id) {
    std::string coin = to_hl_coin(symbol);
    int asset_idx = coin_to_asset_index(coin);
    if (asset_idx < 0) return false;

    nlohmann::json action = {
        {"type", "cancel"},
        {"cancels", nlohmann::json::array({
            {{"a", asset_idx}, {"o", std::stoull(order_id)}}
        })}
    };

    try {
        auto result = http_->exchange_request(action);
        return result.contains("status") && result["status"] == "ok";
    } catch (const std::exception& e) {
        spdlog::error("Cancel order failed: {}", e.what());
        return false;
    }
}

std::vector<OrderResponse> HyperliquidPerp::get_open_orders(const std::string& symbol) {
    auto response = http_->info_request("openOrders");

    std::vector<OrderResponse> orders;
    std::string coin = to_hl_coin(symbol);

    for (const auto& o : response) {
        if (o.value("coin", "") != coin) continue;

        OrderResponse resp;
        resp.order_id = std::to_string(o["oid"].get<uint64_t>());
        resp.symbol = symbol;
        resp.price = std::stod(o["limitPx"].get<std::string>());
        resp.original_qty = std::stod(o["sz"].get<std::string>());
        resp.side = o["side"].get<std::string>() == "B" ? Side::BUY : Side::SELL;
        resp.status = OrderStatus::NEW;
        orders.push_back(resp);
    }

    return orders;
}

FundingInfo HyperliquidPerp::get_funding_info(const std::string& symbol) {
    std::string coin = to_hl_coin(symbol);
    auto response = http_->info_request("metaAndAssetCtxs");

    FundingInfo info;
    info.symbol = symbol;
    info.funding_periods_per_day = 24;  // Hyperliquid: hourly
    info.timestamp_ms = static_cast<uint64_t>(now_ms());

    if (response.is_array() && response.size() >= 2) {
        const auto& meta = response[0]["universe"];
        const auto& ctxs = response[1];

        for (size_t i = 0; i < meta.size() && i < ctxs.size(); i++) {
            if (meta[i]["name"].get<std::string>() == coin) {
                const auto& ctx = ctxs[i];
                info.current_rate = std::stod(ctx.value("funding", "0"));
                info.mark_price = std::stod(ctx.value("markPx", "0"));
                info.index_price = std::stod(ctx.value("oraclePx", "0"));

                // Premium as a proxy for predicted rate
                double premium = std::stod(ctx.value("premium", "0"));
                info.predicted_rate = premium;

                break;
            }
        }
    }

    return info;
}

std::vector<FundingInfo> HyperliquidPerp::get_all_funding_rates() {
    auto response = http_->info_request("metaAndAssetCtxs");

    std::vector<FundingInfo> rates;
    if (!response.is_array() || response.size() < 2) return rates;

    const auto& meta = response[0]["universe"];
    const auto& ctxs = response[1];

    for (size_t i = 0; i < meta.size() && i < ctxs.size(); i++) {
        FundingInfo info;
        info.symbol = from_hl_coin(meta[i]["name"].get<std::string>());
        info.funding_periods_per_day = 24;
        info.timestamp_ms = static_cast<uint64_t>(now_ms());

        const auto& ctx = ctxs[i];
        info.current_rate = std::stod(ctx.value("funding", "0"));
        info.mark_price = std::stod(ctx.value("markPx", "0"));
        info.index_price = std::stod(ctx.value("oraclePx", "0"));
        info.predicted_rate = std::stod(ctx.value("premium", "0"));

        rates.push_back(info);
    }

    return rates;
}

std::vector<FundingInfo> HyperliquidPerp::get_funding_history(const std::string& symbol, int limit) {
    std::string coin = to_hl_coin(symbol);

    uint64_t end_time = static_cast<uint64_t>(now_ms());
    // Each period is 1 hour; fetch `limit` hours of history
    uint64_t start_time = end_time - static_cast<uint64_t>(limit) * 3600 * 1000;

    auto response = http_->info_request("fundingHistory", {
        {"coin", coin},
        {"startTime", start_time},
        {"endTime", end_time}
    });

    std::vector<FundingInfo> history;
    for (const auto& entry : response) {
        FundingInfo info;
        info.symbol = symbol;
        info.funding_periods_per_day = 24;
        info.current_rate = std::stod(entry.value("fundingRate", "0"));
        info.mark_price = std::stod(entry.value("premium", "0"));
        info.timestamp_ms = entry.value("time", 0ULL);
        history.push_back(info);
    }

    // Limit to requested count
    if (static_cast<int>(history.size()) > limit) {
        history.erase(history.begin(), history.begin() + (history.size() - limit));
    }

    return history;
}

bool HyperliquidPerp::set_leverage(const std::string& symbol, int leverage) {
    std::string coin = to_hl_coin(symbol);
    int asset_idx = coin_to_asset_index(coin);
    if (asset_idx < 0) return false;

    nlohmann::json action = {
        {"type", "updateLeverage"},
        {"asset", asset_idx},
        {"isCross", true},
        {"leverage", leverage}
    };

    try {
        auto result = http_->exchange_request(action);
        bool ok = result.contains("status") && result["status"] == "ok";
        if (ok) {
            spdlog::info("Set leverage for {} to {}x", symbol, leverage);
        }
        return ok;
    } catch (const std::exception& e) {
        spdlog::error("Set leverage failed for {}: {}", symbol, e.what());
        return false;
    }
}

std::vector<FuturesInterface::FuturesPosition> HyperliquidPerp::get_positions() {
    auto response = http_->info_request("clearinghouseState");

    std::vector<FuturesPosition> positions;
    if (!response.contains("assetPositions")) return positions;

    for (const auto& ap : response["assetPositions"]) {
        if (!ap.contains("position")) continue;
        const auto& pos = ap["position"];

        FuturesPosition fp;
        fp.symbol = from_hl_coin(pos.value("coin", ""));
        fp.position_amt = std::stod(pos.value("szi", "0"));
        fp.entry_price = std::stod(pos.value("entryPx", "0"));
        fp.unrealized_profit = std::stod(pos.value("unrealizedPnl", "0"));
        if (pos.contains("leverage") && pos["leverage"].is_object()) {
            fp.leverage = std::stod(pos["leverage"].value("value", "1"));
            fp.margin_type = pos["leverage"].value("type", "cross");
        } else {
            fp.leverage = 1;
            fp.margin_type = "cross";
        }

        positions.push_back(fp);
    }

    return positions;
}

// HLP Vault operations
bool HyperliquidPerp::deposit_to_hlp(double amount_usd) {
    nlohmann::json action = {
        {"type", "vaultDeposit"},
        {"vaultAddress", "0x2E2Ed0Cfd3AD2f1d34481277b3204d807Ca2F8c2"},  // HLP vault
        {"usd", static_cast<uint64_t>(amount_usd * 1e6)}  // In micro-USD
    };

    try {
        auto result = http_->exchange_request(action);
        bool ok = result.contains("status") && result["status"] == "ok";
        if (ok) spdlog::info("Deposited ${:.2f} to HLP vault", amount_usd);
        return ok;
    } catch (const std::exception& e) {
        spdlog::error("HLP deposit failed: {}", e.what());
        return false;
    }
}

bool HyperliquidPerp::withdraw_from_hlp(double amount_usd) {
    nlohmann::json action = {
        {"type", "vaultWithdraw"},
        {"vaultAddress", "0x2E2Ed0Cfd3AD2f1d34481277b3204d807Ca2F8c2"},
        {"usd", static_cast<uint64_t>(amount_usd * 1e6)}
    };

    try {
        auto result = http_->exchange_request(action);
        bool ok = result.contains("status") && result["status"] == "ok";
        if (ok) spdlog::info("Withdrew ${:.2f} from HLP vault", amount_usd);
        return ok;
    } catch (const std::exception& e) {
        spdlog::error("HLP withdraw failed: {}", e.what());
        return false;
    }
}

HyperliquidPerp::VaultInfo HyperliquidPerp::get_hlp_info() {
    VaultInfo info;
    try {
        auto response = http_->info_request("vaultDetails", {
            {"vaultAddress", "0x2E2Ed0Cfd3AD2f1d34481277b3204d807Ca2F8c2"}
        });
        if (response.contains("portfolio")) {
            info.total_value = std::stod(response["portfolio"].value("accountValue", "0"));
        }
        // APY estimate from recent performance
        if (response.contains("apr")) {
            info.apy_estimate = response["apr"].get<double>() * 100.0;
        }
    } catch (const std::exception& e) {
        spdlog::warn("Failed to get HLP info: {}", e.what());
    }
    return info;
}

} // namespace arb
