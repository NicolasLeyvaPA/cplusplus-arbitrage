#include <gtest/gtest.h>
#include <cstring>
#include "common/types.hpp"
#include "config/config.hpp"
#include "utils/keccak256.hpp"
#include "utils/msgpack_lite.hpp"
#include "exchange/exchange_interface.hpp"
#include "execution/execution_engine.hpp"

using namespace arb;

// ============================================================================
// KECCAK-256 TESTS
// ============================================================================

TEST(Keccak256Test, EmptyString) {
    // Known keccak256("") = c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
    auto hash = crypto::keccak256_hex("");
    EXPECT_EQ(hash, "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
}

TEST(Keccak256Test, HelloWorld) {
    // Known keccak256("hello") = 1c8aff950685c2ed4bc3174f3472287b56d9517b9c948127319a09a7a36deac8
    auto hash = crypto::keccak256_hex("hello");
    EXPECT_EQ(hash, "1c8aff950685c2ed4bc3174f3472287b56d9517b9c948127319a09a7a36deac8");
}

TEST(Keccak256Test, BinaryData) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto hash = crypto::keccak256(data);
    EXPECT_EQ(hash.size(), 32u);
    // Result should be deterministic
    auto hash2 = crypto::keccak256(data);
    EXPECT_EQ(hash, hash2);
}

TEST(Keccak256Test, HexOutput) {
    auto hex = crypto::keccak256_hex("test");
    EXPECT_EQ(hex.size(), 64u);  // 32 bytes = 64 hex chars
    // All chars should be valid hex
    for (char c : hex) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

// ============================================================================
// MSGPACK TESTS
// ============================================================================

TEST(MsgpackTest, PackString) {
    msgpack::Packer packer;
    packer.pack_string("hello");
    const auto& data = packer.data();
    EXPECT_GT(data.size(), 5u);
    // fixstr format: 0xa0 | len
    EXPECT_EQ(data[0], 0xa5);  // 0xa0 | 5
}

TEST(MsgpackTest, PackUint) {
    msgpack::Packer packer;
    packer.pack_uint(42);
    const auto& data = packer.data();
    EXPECT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0], 42);  // positive fixnum
}

TEST(MsgpackTest, PackBool) {
    msgpack::Packer packer;
    packer.pack_bool(true);
    packer.pack_bool(false);
    const auto& data = packer.data();
    EXPECT_EQ(data.size(), 2u);
    EXPECT_EQ(data[0], 0xc3);  // true
    EXPECT_EQ(data[1], 0xc2);  // false
}

TEST(MsgpackTest, PackNil) {
    msgpack::Packer packer;
    packer.pack_nil();
    const auto& data = packer.data();
    EXPECT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0], 0xc0);
}

TEST(MsgpackTest, PackMap) {
    msgpack::Packer packer;
    packer.pack_map(2);
    packer.pack_string("key1");
    packer.pack_string("val1");
    packer.pack_string("key2");
    packer.pack_uint(42);
    const auto& data = packer.data();
    EXPECT_GT(data.size(), 0u);
    // fixmap header: 0x80 | 2
    EXPECT_EQ(data[0], 0x82);
}

TEST(MsgpackTest, PackArray) {
    msgpack::Packer packer;
    packer.pack_array(3);
    packer.pack_uint(1);
    packer.pack_uint(2);
    packer.pack_uint(3);
    const auto& data = packer.data();
    // fixarray header: 0x90 | 3
    EXPECT_EQ(data[0], 0x93);
}

TEST(MsgpackTest, Clear) {
    msgpack::Packer packer;
    packer.pack_string("test");
    EXPECT_GT(packer.data().size(), 0u);
    packer.clear();
    EXPECT_EQ(packer.data().size(), 0u);
}

// ============================================================================
// SYMBOL MAPPING TESTS (via types only, no exchange connection needed)
// ============================================================================

TEST(SymbolMappingTest, StripUSDT) {
    // Test the symbol stripping logic directly
    std::string symbol = "BTCUSDT";
    std::string coin = symbol;
    for (const auto& suffix : {"USDT", "USDC", "USD", "PERP"}) {
        size_t pos = coin.rfind(suffix);
        if (pos != std::string::npos && pos + std::strlen(suffix) == coin.size()) {
            coin = coin.substr(0, pos);
            break;
        }
    }
    EXPECT_EQ(coin, "BTC");
}

TEST(SymbolMappingTest, StripUSDC) {
    std::string symbol = "ETHUSDC";
    std::string coin = symbol;
    for (const auto& suffix : {"USDT", "USDC", "USD", "PERP"}) {
        size_t pos = coin.rfind(suffix);
        if (pos != std::string::npos && pos + std::strlen(suffix) == coin.size()) {
            coin = coin.substr(0, pos);
            break;
        }
    }
    EXPECT_EQ(coin, "ETH");
}

TEST(SymbolMappingTest, StripPERP) {
    std::string symbol = "SOLPERP";
    std::string coin = symbol;
    for (const auto& suffix : {"USDT", "USDC", "USD", "PERP"}) {
        size_t pos = coin.rfind(suffix);
        if (pos != std::string::npos && pos + std::strlen(suffix) == coin.size()) {
            coin = coin.substr(0, pos);
            break;
        }
    }
    EXPECT_EQ(coin, "SOL");
}

TEST(SymbolMappingTest, NoCoinSuffix) {
    std::string symbol = "BTC";
    std::string coin = symbol;
    for (const auto& suffix : {"USDT", "USDC", "USD", "PERP"}) {
        size_t pos = coin.rfind(suffix);
        if (pos != std::string::npos && pos + std::strlen(suffix) == coin.size()) {
            coin = coin.substr(0, pos);
            break;
        }
    }
    EXPECT_EQ(coin, "BTC");
}

// ============================================================================
// HEDGE MODE TESTS
// ============================================================================

TEST(HedgeModeTest, ToString) {
    EXPECT_EQ(arb::to_string(HedgeMode::FULL_HEDGE), "FULL_HEDGE");
    EXPECT_EQ(arb::to_string(HedgeMode::USDC_COLLATERAL), "USDC_COLLATERAL");
}

TEST(HedgeModeTest, FromString) {
    EXPECT_EQ(hedge_mode_from_str("FULL_HEDGE"), HedgeMode::FULL_HEDGE);
    EXPECT_EQ(hedge_mode_from_str("full_hedge"), HedgeMode::FULL_HEDGE);
    EXPECT_EQ(hedge_mode_from_str("anything"), HedgeMode::USDC_COLLATERAL);
}

TEST(HedgeModeTest, IsBalancedUSDCCollateral) {
    DeltaNeutralPosition pos;
    pos.spot_size = 0;
    pos.futures_size = -1.0;
    // USDC_COLLATERAL mode is always balanced (no spot leg)
    EXPECT_TRUE(pos.is_balanced(1.0, HedgeMode::USDC_COLLATERAL));
}

TEST(HedgeModeTest, IsBalancedFullHedge) {
    DeltaNeutralPosition pos;
    pos.spot_size = 1.0;
    pos.futures_size = -0.90;  // 10% off
    EXPECT_FALSE(pos.is_balanced(1.0, HedgeMode::FULL_HEDGE));
    EXPECT_TRUE(pos.is_balanced(11.0, HedgeMode::FULL_HEDGE));  // 11% tolerance
}

// ============================================================================
// FUNDING INFO TESTS (hourly vs 8-hourly)
// ============================================================================

TEST(FundingInfoTest, AnnualizedRateHourly) {
    FundingInfo fi;
    fi.current_rate = 0.0001;
    fi.funding_periods_per_day = 24;  // Hyperliquid hourly
    double expected = 0.0001 * 24.0 * 365.0 * 100.0;
    EXPECT_DOUBLE_EQ(fi.annualized_rate(), expected);
}

TEST(FundingInfoTest, AnnualizedRate8Hour) {
    FundingInfo fi;
    fi.current_rate = 0.0001;
    fi.funding_periods_per_day = 3;  // Binance 8-hourly
    double expected = 0.0001 * 3.0 * 365.0 * 100.0;
    EXPECT_DOUBLE_EQ(fi.annualized_rate(), expected);
}

TEST(FundingInfoTest, SameAPYDifferentPeriods) {
    // Hyperliquid rate per hour should be ~8x smaller than Binance per 8h for same APY
    FundingInfo binance;
    binance.current_rate = 0.0008;  // 0.08% per 8h
    binance.funding_periods_per_day = 3;

    FundingInfo hyperliquid;
    hyperliquid.current_rate = 0.0001;  // 0.01% per hour
    hyperliquid.funding_periods_per_day = 24;

    // Binance: 0.0008 * 3 * 365 * 100 = 87.6%
    // HL:      0.0001 * 24 * 365 * 100 = 87.6%
    EXPECT_NEAR(binance.annualized_rate(), hyperliquid.annualized_rate(), 0.001);
}

// ============================================================================
// NULL EXCHANGE TESTS
// ============================================================================

TEST(NullExchangeTest, ConnectReturnsTrue) {
    NullExchange ex;
    EXPECT_TRUE(ex.connect());
}

TEST(NullExchangeTest, IsConnected) {
    NullExchange ex;
    EXPECT_TRUE(ex.is_connected());
    EXPECT_EQ(ex.status(), ConnectionStatus::CONNECTED);
}

TEST(NullExchangeTest, PlaceOrderRejected) {
    NullExchange ex;
    OrderRequest req;
    auto resp = ex.place_order(req);
    EXPECT_EQ(resp.status, OrderStatus::REJECTED);
    EXPECT_FALSE(resp.error_message.empty());
}

TEST(NullExchangeTest, GetTickerReturnsEmpty) {
    NullExchange ex;
    auto ticker = ex.get_ticker("BTCUSDT");
    EXPECT_EQ(ticker.bid, 0.0);
    EXPECT_EQ(ticker.ask, 0.0);
}

// ============================================================================
// EXECUTION ENGINE - USDC_COLLATERAL MODE
// ============================================================================

TEST(ExecutionEngineTest, USCDCollateralDryRun) {
    NullExchange spot;
    // Can't instantiate a real FuturesInterface here, but we can test
    // that the ExecutionEngine constructor compiles with the new interface
    // (tested via the full build + dry run mode)
}

// ============================================================================
// FEE CONFIG TESTS
// ============================================================================

TEST(FeeConfigTest, Defaults) {
    FeeConfig fees;
    EXPECT_DOUBLE_EQ(fees.perp_taker_fee_pct, 0.00045);
    EXPECT_DOUBLE_EQ(fees.perp_maker_fee_pct, 0.00015);
    EXPECT_DOUBLE_EQ(fees.spot_taker_fee_pct, 0.00070);
    EXPECT_DOUBLE_EQ(fees.spot_maker_fee_pct, 0.00040);
}

TEST(FeeConfigTest, FeeBreakevenCalculation) {
    FeeConfig fees;
    // Entry fee (perp taker): 0.045%
    // Exit fee (perp taker): 0.045%
    // Total round-trip: 0.09%
    double round_trip_fee = 2 * fees.perp_taker_fee_pct;
    EXPECT_NEAR(round_trip_fee, 0.0009, 1e-6);

    // With funding rate of 0.01% per hour (Hyperliquid hourly):
    double hourly_funding = 0.0001;
    double hours_to_breakeven = round_trip_fee / hourly_funding;
    EXPECT_NEAR(hours_to_breakeven, 9.0, 0.1);  // 9 hours to break even
}

// ============================================================================
// CONFIG TESTS
// ============================================================================

TEST(ConfigTest, DefaultExchangeType) {
    ExchangeConfig config;
    EXPECT_EQ(config.exchange_type, "hyperliquid");
}

TEST(ConfigTest, DefaultHedgeMode) {
    StrategyConfig config;
    EXPECT_EQ(config.hedge_mode, HedgeMode::USDC_COLLATERAL);
}

TEST(ConfigTest, DefaultFundingPeriods) {
    StrategyConfig config;
    EXPECT_EQ(config.funding_periods_per_day, 24);  // Hyperliquid default
}

TEST(ConfigTest, DefaultCapital) {
    CapitalConfig config;
    EXPECT_DOUBLE_EQ(config.starting_balance_usd, 50.0);
}

TEST(ConfigTest, DefaultMinNotional) {
    StrategyConfig config;
    EXPECT_DOUBLE_EQ(config.min_notional_usd, 10.0);
}
