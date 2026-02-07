#include <gtest/gtest.h>
#include "position/position_manager.hpp"

using namespace arb;

TEST(PositionManagerTest, OpenAndGetPosition) {
    PositionManager pm;

    DeltaNeutralPosition pos;
    pos.symbol = "BTCUSDT";
    pos.spot_size = 0.1;
    pos.futures_size = -0.1;
    pm.open_position("BTCUSDT", pos);

    auto result = pm.get_position("BTCUSDT");
    EXPECT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->spot_size, 0.1);
    EXPECT_DOUBLE_EQ(result->futures_size, -0.1);
}

TEST(PositionManagerTest, GetNonexistentPosition) {
    PositionManager pm;
    auto result = pm.get_position("BTCUSDT");
    EXPECT_FALSE(result.has_value());
}

TEST(PositionManagerTest, UpdateSpotFill) {
    PositionManager pm;
    pm.update_spot_fill("BTCUSDT", 0.1, 50000.0, 2.0);

    auto pos = pm.get_position("BTCUSDT");
    EXPECT_TRUE(pos.has_value());
    EXPECT_DOUBLE_EQ(pos->spot_size, 0.1);
    EXPECT_DOUBLE_EQ(pos->spot_avg_entry, 50000.0);
    EXPECT_DOUBLE_EQ(pos->total_fees, 2.0);
}

TEST(PositionManagerTest, UpdateFuturesFill) {
    PositionManager pm;
    pm.update_futures_fill("BTCUSDT", -0.1, 50100.0, 2.0);

    auto pos = pm.get_position("BTCUSDT");
    EXPECT_TRUE(pos.has_value());
    EXPECT_DOUBLE_EQ(pos->futures_size, -0.1);
    EXPECT_DOUBLE_EQ(pos->total_fees, 2.0);
}

TEST(PositionManagerTest, ClosePosition) {
    PositionManager pm;
    DeltaNeutralPosition pos;
    pos.symbol = "BTCUSDT";
    pos.spot_size = 0.1;
    pm.open_position("BTCUSDT", pos);

    pm.close_position("BTCUSDT");
    auto result = pm.get_position("BTCUSDT");
    EXPECT_FALSE(result.has_value());
}

TEST(PositionManagerTest, RecordFunding) {
    PositionManager pm;
    DeltaNeutralPosition pos;
    pos.symbol = "BTCUSDT";
    pos.spot_size = 0.1;
    pos.futures_size = -0.1;
    pm.open_position("BTCUSDT", pos);

    pm.record_funding("BTCUSDT", 1.5);

    auto result = pm.get_position("BTCUSDT");
    EXPECT_DOUBLE_EQ(result->funding_collected, 1.5);
    EXPECT_EQ(result->funding_periods, 1);
}

TEST(PositionManagerTest, TotalPnl) {
    PositionManager pm;

    DeltaNeutralPosition pos1;
    pos1.symbol = "BTCUSDT";
    pos1.spot_size = 0.1;
    pos1.realized_pnl = 10.0;
    pos1.funding_collected = 5.0;
    pm.open_position("BTCUSDT", pos1);

    DeltaNeutralPosition pos2;
    pos2.symbol = "ETHUSDT";
    pos2.spot_size = 1.0;
    pos2.realized_pnl = 3.0;
    pos2.funding_collected = 2.0;
    pm.open_position("ETHUSDT", pos2);

    EXPECT_DOUBLE_EQ(pm.total_pnl(), 20.0);  // 15 + 5
}

TEST(PositionManagerTest, TotalFunding) {
    PositionManager pm;

    DeltaNeutralPosition pos;
    pos.symbol = "BTCUSDT";
    pos.spot_size = 0.1;
    pos.funding_collected = 7.5;
    pm.open_position("BTCUSDT", pos);

    EXPECT_DOUBLE_EQ(pm.total_funding(), 7.5);
}

TEST(PositionManagerTest, OpenPositionCount) {
    PositionManager pm;

    DeltaNeutralPosition pos1;
    pos1.symbol = "BTCUSDT";
    pos1.spot_size = 0.1;
    pm.open_position("BTCUSDT", pos1);

    DeltaNeutralPosition pos2;
    pos2.symbol = "ETHUSDT";
    pos2.spot_size = 1.0;
    pm.open_position("ETHUSDT", pos2);

    EXPECT_EQ(pm.open_position_count(), 2);
}

TEST(PositionManagerTest, GetOpenSymbols) {
    PositionManager pm;

    DeltaNeutralPosition pos;
    pos.symbol = "BTCUSDT";
    pos.spot_size = 0.1;
    pm.open_position("BTCUSDT", pos);

    auto symbols = pm.get_open_symbols();
    EXPECT_EQ(symbols.size(), 1);
    EXPECT_EQ(symbols[0], "BTCUSDT");
}
