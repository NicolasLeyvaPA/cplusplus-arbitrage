#pragma once

#include "exchange/exchange_interface.hpp"
#include "exchange/hyperliquid_client.hpp"
#include "exchange/hyperliquid_signer.hpp"
#include "config/config.hpp"
#include <atomic>
#include <memory>
#include <map>
#include <mutex>

namespace arb {

/**
 * Hyperliquid perpetual futures exchange client.
 * Implements FuturesInterface for the Hyperliquid DEX.
 *
 * Key differences from Binance:
 *   - Funding is hourly (24 periods/day)
 *   - Symbols are bare coin names ("BTC", "ETH") mapped from internal "BTCUSDT"
 *   - All reads: POST /info with JSON body
 *   - All writes: POST /exchange with EIP-712 signed payload
 *   - Asset indices needed for orders (BTC=0, ETH=1, etc.)
 */
class HyperliquidPerp : public FuturesInterface {
public:
    explicit HyperliquidPerp(const ExchangeConfig& config);

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

    // FuturesInterface
    FundingInfo get_funding_info(const std::string& symbol) override;
    std::vector<FundingInfo> get_all_funding_rates() override;
    std::vector<FundingInfo> get_funding_history(const std::string& symbol, int limit = 100) override;
    bool set_leverage(const std::string& symbol, int leverage) override;
    std::vector<FuturesPosition> get_positions() override;

    // HLP vault operations
    struct VaultInfo {
        double total_value{0};
        double user_share{0};
        double apy_estimate{0};
    };
    bool deposit_to_hlp(double amount_usd);
    bool withdraw_from_hlp(double amount_usd);
    VaultInfo get_hlp_info();

    // Symbol mapping
    std::string to_hl_coin(const std::string& internal_symbol) const;
    std::string from_hl_coin(const std::string& hl_coin) const;
    int coin_to_asset_index(const std::string& coin) const;

private:
    void load_asset_metadata();

    struct AssetMeta {
        int index{-1};
        std::string name;
        int sz_decimals{0};     // Size precision
        int price_decimals{0};  // Price precision (for order formatting)
        double min_size{0};
    };

    std::unique_ptr<HyperliquidSigner> signer_;
    std::unique_ptr<HyperliquidHttpClient> http_;
    ExchangeConfig config_;

    std::atomic<ConnectionStatus> status_{ConnectionStatus::DISCONNECTED};
    mutable std::mutex meta_mutex_;
    std::map<std::string, AssetMeta> asset_meta_;  // keyed by coin name ("BTC")
    bool meta_loaded_{false};
};

} // namespace arb
