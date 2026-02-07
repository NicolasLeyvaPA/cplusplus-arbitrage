#include <gtest/gtest.h>
#include "common/types.hpp"

using namespace arb;

TEST(TickerTest, MidPrice) {
    Ticker t;
    t.bid = 100.0;
    t.ask = 102.0;
    EXPECT_DOUBLE_EQ(t.mid(), 101.0);
}

TEST(TickerTest, Spread) {
    Ticker t;
    t.bid = 100.0;
    t.ask = 102.0;
    EXPECT_DOUBLE_EQ(t.spread(), 2.0);
}

TEST(TickerTest, SpreadBps) {
    Ticker t;
    t.bid = 100.0;
    t.ask = 101.0;
    double expected_bps = 1.0 / 100.5 * 10000.0;
    EXPECT_NEAR(t.spread_bps(), expected_bps, 0.01);
}

TEST(FundingInfoTest, AnnualizedRate) {
    FundingInfo fi;
    fi.current_rate = 0.0001;  // 0.01% per 8h
    // 3 periods/day * 365 days * 100 = annualized %
    double expected = 0.0001 * 3.0 * 365.0 * 100.0;
    EXPECT_DOUBLE_EQ(fi.annualized_rate(), expected);
}

TEST(DeltaNeutralPositionTest, NetDelta) {
    DeltaNeutralPosition pos;
    pos.spot_size = 1.0;
    pos.futures_size = -1.0;
    EXPECT_DOUBLE_EQ(pos.net_delta(), 0.0);
}

TEST(DeltaNeutralPositionTest, NetDeltaImbalanced) {
    DeltaNeutralPosition pos;
    pos.spot_size = 1.0;
    pos.futures_size = -0.95;
    EXPECT_NEAR(pos.net_delta(), 0.05, 1e-10);
}

TEST(DeltaNeutralPositionTest, IsBalanced) {
    DeltaNeutralPosition pos;
    pos.spot_size = 1.0;
    pos.futures_size = -1.0;
    EXPECT_TRUE(pos.is_balanced());
}

TEST(DeltaNeutralPositionTest, IsNotBalanced) {
    DeltaNeutralPosition pos;
    pos.spot_size = 1.0;
    pos.futures_size = -0.90;  // 10% off
    EXPECT_FALSE(pos.is_balanced(1.0));  // 1% threshold
}

TEST(DeltaNeutralPositionTest, TotalPnl) {
    DeltaNeutralPosition pos;
    pos.realized_pnl = 10.0;
    pos.unrealized_pnl = 5.0;
    pos.funding_collected = 3.0;
    pos.total_fees = 2.0;
    // total_pnl = realized + unrealized + funding - fees
    EXPECT_DOUBLE_EQ(pos.total_pnl(), 16.0);
}

TEST(DeltaNeutralPositionTest, TotalNotional) {
    DeltaNeutralPosition pos;
    pos.spot_notional = 5000.0;
    pos.futures_notional = -5000.0;
    EXPECT_DOUBLE_EQ(pos.total_notional(), 10000.0);
}

TEST(DeltaNeutralPositionTest, IsOpen) {
    DeltaNeutralPosition pos;
    EXPECT_FALSE(pos.is_open());

    pos.spot_size = 1.0;
    EXPECT_TRUE(pos.is_open());
}

TEST(StringConversionsTest, SideToString) {
    EXPECT_EQ(arb::to_string(Side::BUY), "BUY");
    EXPECT_EQ(arb::to_string(Side::SELL), "SELL");
}

TEST(StringConversionsTest, TradingModeToString) {
    EXPECT_EQ(arb::to_string(TradingMode::DRY_RUN), "DRY_RUN");
    EXPECT_EQ(arb::to_string(TradingMode::PAPER), "PAPER");
    EXPECT_EQ(arb::to_string(TradingMode::LIVE), "LIVE");
}

TEST(StringConversionsTest, TradingModeFromString) {
    EXPECT_EQ(trading_mode_from_str("LIVE"), TradingMode::LIVE);
    EXPECT_EQ(trading_mode_from_str("live"), TradingMode::LIVE);
    EXPECT_EQ(trading_mode_from_str("PAPER"), TradingMode::PAPER);
    EXPECT_EQ(trading_mode_from_str("anything"), TradingMode::DRY_RUN);
}

TEST(OrderResponseTest, SuccessCheck) {
    OrderResponse resp;
    resp.status = OrderStatus::FILLED;
    EXPECT_TRUE(resp.success());

    resp.error_message = "some error";
    EXPECT_FALSE(resp.success());

    resp.error_message.clear();
    resp.status = OrderStatus::REJECTED;
    EXPECT_FALSE(resp.success());
}
