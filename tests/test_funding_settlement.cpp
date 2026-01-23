#include <gtest/gtest.h>
#include "arbitrage/funding_settlement_engine.hpp"
#include "arbitrage/delta_neutral_enforcer.hpp"
#include "arbitrage/demo_trading_harness.hpp"
#include <filesystem>

using namespace arb;

class FundingSettlementTest : public ::testing::Test {
protected:
    std::string test_db_path_;
    std::shared_ptr<SessionDatabase> db_;
    std::string session_id_;

    void SetUp() override {
        test_db_path_ = "/tmp/test_funding_" + generate_uuid() + ".db";
        db_ = std::make_shared<SessionDatabase>(test_db_path_);
        db_->initialize_schema();

        Session session;
        session.starting_balance = 10000;
        session.mode = TradingMode::DEMO;
        session_id_ = db_->create_session(session);
    }

    void TearDown() override {
        db_.reset();
        if (std::filesystem::exists(test_db_path_)) {
            std::filesystem::remove(test_db_path_);
        }
        std::filesystem::remove(test_db_path_ + "-wal");
        std::filesystem::remove(test_db_path_ + "-shm");
    }
};

// ============================================================================
// Funding Settlement Engine Tests
// ============================================================================

TEST_F(FundingSettlementTest, UpdateAndGetFundingRate) {
    FundingSettlementEngine engine(db_, session_id_);

    FundingRate rate;
    rate.venue = "binance";
    rate.instrument = "BTCUSDT";
    rate.rate = 0.0001;
    rate.mark_price = 45000;

    engine.update_funding_rate(rate);

    auto retrieved = engine.get_funding_rate("binance", "BTCUSDT");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_DOUBLE_EQ(retrieved->rate, 0.0001);
    EXPECT_DOUBLE_EQ(retrieved->mark_price, 45000);
}

TEST_F(FundingSettlementTest, SettleFunding_ShortReceivesFunding) {
    FundingSettlementEngine engine(db_, session_id_);

    FundingRate rate;
    rate.venue = "binance";
    rate.instrument = "BTCUSDT";
    rate.rate = 0.0001;  // Positive: longs pay shorts
    rate.mark_price = 45000;

    engine.update_funding_rate(rate);

    // Short position should receive funding
    double payment = engine.settle_funding("binance", "BTCUSDT", -0.1, 45000);

    // Payment = -position_qty * mark_price * rate
    // = -(-0.1) * 45000 * 0.0001 = 0.45
    EXPECT_NEAR(payment, 0.45, 0.001);

    auto stats = engine.stats();
    EXPECT_EQ(stats.funding_settlements, 1);
    EXPECT_NEAR(stats.total_funding_received, 0.45, 0.001);
}

TEST_F(FundingSettlementTest, SettleFunding_LongPaysFunding) {
    FundingSettlementEngine engine(db_, session_id_);

    FundingRate rate;
    rate.venue = "binance";
    rate.instrument = "BTCUSDT";
    rate.rate = 0.0001;  // Positive: longs pay shorts
    rate.mark_price = 45000;

    engine.update_funding_rate(rate);

    // Long position should pay funding
    double payment = engine.settle_funding("binance", "BTCUSDT", 0.1, 45000);

    // Payment = -position_qty * mark_price * rate
    // = -(0.1) * 45000 * 0.0001 = -0.45
    EXPECT_NEAR(payment, -0.45, 0.001);

    auto stats = engine.stats();
    EXPECT_EQ(stats.funding_settlements, 1);
    EXPECT_NEAR(stats.total_funding_paid, 0.45, 0.001);
}

TEST_F(FundingSettlementTest, SettleFunding_NegativeRate) {
    FundingSettlementEngine engine(db_, session_id_);

    FundingRate rate;
    rate.venue = "binance";
    rate.instrument = "BTCUSDT";
    rate.rate = -0.0001;  // Negative: shorts pay longs
    rate.mark_price = 45000;

    engine.update_funding_rate(rate);

    // Short position should pay when rate is negative
    double short_payment = engine.settle_funding("binance", "BTCUSDT", -0.1, 45000);
    EXPECT_NEAR(short_payment, -0.45, 0.001);

    // Update rate again for long test
    engine.update_funding_rate(rate);

    // Long position should receive when rate is negative
    double long_payment = engine.settle_funding("binance", "BTCUSDT", 0.1, 45000);
    EXPECT_NEAR(long_payment, 0.45, 0.001);
}

TEST_F(FundingSettlementTest, SettleAllFunding) {
    FundingSettlementEngine engine(db_, session_id_);

    // Set up funding rates
    FundingRate binance_rate;
    binance_rate.venue = "binance";
    binance_rate.instrument = "BTCUSDT";
    binance_rate.rate = 0.0002;  // Higher funding
    binance_rate.mark_price = 45000;
    binance_rate.next_funding_time = 1;  // In the past, so due now

    FundingRate bybit_rate;
    bybit_rate.venue = "bybit";
    bybit_rate.instrument = "BTCUSDT";
    bybit_rate.rate = 0.0001;  // Lower funding
    bybit_rate.mark_price = 45000;
    bybit_rate.next_funding_time = 1;

    engine.update_funding_rate(binance_rate);
    engine.update_funding_rate(bybit_rate);

    // Positions: short binance, long bybit (classic funding arb)
    std::vector<Position> positions;
    Position short_pos;
    short_pos.venue = "binance";
    short_pos.instrument = "BTCUSDT";
    short_pos.qty = -0.1;
    short_pos.avg_price = 45000;
    positions.push_back(short_pos);

    Position long_pos;
    long_pos.venue = "bybit";
    long_pos.instrument = "BTCUSDT";
    long_pos.qty = 0.1;
    long_pos.avg_price = 45000;
    positions.push_back(long_pos);

    double total = engine.settle_all_funding(positions);

    // Short binance receives: 0.1 * 45000 * 0.0002 = 0.90
    // Long bybit pays: -0.1 * 45000 * 0.0001 = -0.45
    // Net = 0.90 - 0.45 = 0.45
    EXPECT_NEAR(total, 0.45, 0.01);
}

// ============================================================================
// Fill Simulation Tests
// ============================================================================

TEST_F(FundingSettlementTest, SimulateFill_BasicMarketOrder) {
    DemoFillConfig config;
    config.base_fill_probability = 1.0;  // Always fill for this test
    config.slippage_mean_bps = 0;
    config.slippage_stddev_bps = 0;

    FundingSettlementEngine engine(db_, session_id_, config);

    auto result = engine.simulate_fill(
        "binance", "BTCUSDT",
        OrderSide::BUY, 0.1,
        0,  // Market order (no limit)
        44990, 45000,  // bid, ask
        1.0, 1.0       // bid_qty, ask_qty
    );

    EXPECT_TRUE(result.filled);
    EXPECT_NEAR(result.fill_qty, 0.1, 0.001);
    // With 0 slippage, should fill near ask
    EXPECT_GE(result.fill_price, 44990);
    EXPECT_LE(result.fill_price, 45100);  // Some tolerance
}

TEST_F(FundingSettlementTest, SimulateFill_AdversarialNonFill) {
    DemoFillConfig config;
    config.base_fill_probability = 0.0;  // Never fill
    config.min_fill_probability = 0.0;

    FundingSettlementEngine engine(db_, session_id_, config);

    auto result = engine.simulate_fill(
        "binance", "BTCUSDT",
        OrderSide::BUY, 0.1,
        0, 44990, 45000, 1.0, 1.0
    );

    EXPECT_FALSE(result.filled);
    EXPECT_FALSE(result.rejection_reason.empty());
}

TEST_F(FundingSettlementTest, SimulateFill_LimitPriceRejection) {
    DemoFillConfig config;
    config.base_fill_probability = 1.0;
    config.slippage_mean_bps = 10;  // Guaranteed slippage

    FundingSettlementEngine engine(db_, session_id_, config);

    // Try to buy with limit below ask
    auto result = engine.simulate_fill(
        "binance", "BTCUSDT",
        OrderSide::BUY, 0.1,
        44980,  // Limit below ask of 45000
        44990, 45000, 1.0, 1.0
    );

    // Should reject because fill price would exceed limit
    EXPECT_FALSE(result.filled);
    EXPECT_TRUE(result.rejection_reason.find("Limit") != std::string::npos);
}

TEST_F(FundingSettlementTest, SimulateFill_TracksStats) {
    DemoFillConfig config;
    config.base_fill_probability = 1.0;
    config.max_fill_probability = 1.0;  // Override the cap

    FundingSettlementEngine engine(db_, session_id_, config);

    for (int i = 0; i < 10; i++) {
        engine.simulate_fill(
            "binance", "BTCUSDT",
            OrderSide::BUY, 0.1,
            0, 44990, 45000, 1.0, 1.0
        );
    }

    auto stats = engine.stats();
    EXPECT_EQ(stats.orders_simulated, 10);
    // Fill probability is affected by spread and size, so allow some variance
    EXPECT_GE(stats.orders_filled, 8);  // At least 80% should fill
    EXPECT_GT(stats.total_fees_paid, 0);
}

// ============================================================================
// Delta-Neutral Enforcer Tests
// ============================================================================

class DeltaNeutralTest : public FundingSettlementTest {};

TEST_F(DeltaNeutralTest, ComputeDelta_Neutral) {
    DeltaNeutralEnforcer enforcer(db_, session_id_);

    Position long_pos;
    long_pos.venue = "binance";
    long_pos.instrument = "BTCUSDT";
    long_pos.qty = 0.1;
    long_pos.avg_price = 45000;
    long_pos.mark_price = 45000;

    Position short_pos;
    short_pos.venue = "bybit";
    short_pos.instrument = "BTCUSDT";
    short_pos.qty = -0.1;
    short_pos.avg_price = 45000;
    short_pos.mark_price = 45000;

    enforcer.update_position(long_pos);
    enforcer.update_position(short_pos);

    auto delta = enforcer.compute_delta();

    EXPECT_NEAR(delta.total_long_notional, 4500, 1);
    EXPECT_NEAR(delta.total_short_notional, 4500, 1);
    EXPECT_NEAR(delta.net_delta, 0, 1);
    EXPECT_TRUE(delta.is_neutral);
}

TEST_F(DeltaNeutralTest, ComputeDelta_NotNeutral) {
    DeltaNeutralConfig config;
    config.max_delta_notional = 100;
    DeltaNeutralEnforcer enforcer(db_, session_id_, config);

    Position long_pos;
    long_pos.venue = "binance";
    long_pos.instrument = "BTCUSDT";
    long_pos.qty = 0.15;  // Larger long
    long_pos.avg_price = 45000;
    long_pos.mark_price = 45000;

    Position short_pos;
    short_pos.venue = "bybit";
    short_pos.instrument = "BTCUSDT";
    short_pos.qty = -0.1;  // Smaller short
    short_pos.avg_price = 45000;
    short_pos.mark_price = 45000;

    enforcer.update_position(long_pos);
    enforcer.update_position(short_pos);

    auto delta = enforcer.compute_delta();

    // Net delta = 0.15*45000 - 0.1*45000 = 6750 - 4500 = 2250
    EXPECT_NEAR(delta.net_delta, 2250, 10);
    EXPECT_FALSE(delta.is_neutral);
}

TEST_F(DeltaNeutralTest, HedgeTriggered) {
    DeltaNeutralConfig config;
    config.delta_hedge_threshold = 100;
    DeltaNeutralEnforcer enforcer(db_, session_id_, config);

    Position long_pos;
    long_pos.venue = "binance";
    long_pos.instrument = "BTCUSDT";
    long_pos.qty = 0.1;
    long_pos.avg_price = 45000;
    long_pos.mark_price = 45000;

    // No short - major delta imbalance
    enforcer.update_position(long_pos);
    enforcer.update_mark_price("binance", "BTCUSDT", 45000, 45000);

    auto hedges = enforcer.check_hedge_needed();

    EXPECT_FALSE(hedges.empty());
    EXPECT_EQ(hedges[0].side, OrderSide::SELL);  // Need to sell to reduce long delta
}

TEST_F(DeltaNeutralTest, KillCondition_BasisDivergence) {
    DeltaNeutralConfig config;
    config.basis_divergence_limit = 0.005;  // 0.5%
    DeltaNeutralEnforcer enforcer(db_, session_id_, config);

    // Binance: mark = 45000, index = 44800 (basis = 0.45%)
    // Bybit: mark = 45000, index = 45500 (basis = -1.1%)
    // Divergence = 0.45% - (-1.1%) = 1.55% > 0.5% limit
    enforcer.update_mark_price("binance", "BTCUSDT", 45000, 44800);
    enforcer.update_mark_price("bybit", "BTCUSDT", 45000, 45500);

    auto kill = enforcer.check_basis_divergence();

    ASSERT_TRUE(kill.has_value());
    EXPECT_EQ(kill->reason, KillReason::BASIS_DIVERGENCE);
    EXPECT_TRUE(kill->should_kill);
}

TEST_F(DeltaNeutralTest, KillCondition_FundingFlip) {
    DeltaNeutralConfig config;
    config.funding_flip_buffer = 0.00001;
    DeltaNeutralEnforcer enforcer(db_, session_id_, config);

    // Set up position
    Position short_pos;
    short_pos.venue = "binance";
    short_pos.instrument = "BTCUSDT";
    short_pos.qty = -0.1;
    enforcer.update_position(short_pos);

    // Initial: positive funding on short = receiving funding
    enforcer.update_funding_rate("binance", 0.0002);
    auto kill1 = enforcer.check_funding_flip();
    EXPECT_FALSE(kill1.has_value());

    // Flip: negative funding on short = paying funding
    enforcer.update_funding_rate("binance", -0.0002);
    auto kill2 = enforcer.check_funding_flip();
    ASSERT_TRUE(kill2.has_value());
    EXPECT_EQ(kill2->reason, KillReason::FUNDING_FLIP);
}

TEST_F(DeltaNeutralTest, KillCondition_SpreadCollapse) {
    DeltaNeutralConfig config;
    config.min_funding_spread = 0.0001;
    DeltaNeutralEnforcer enforcer(db_, session_id_, config);

    // Initial good spread
    enforcer.update_funding_rate("binance", 0.0003);
    enforcer.update_funding_rate("bybit", 0.0001);
    auto kill1 = enforcer.check_spread_collapse();
    EXPECT_FALSE(kill1.has_value());

    // Spread collapses
    enforcer.update_funding_rate("binance", 0.00011);
    enforcer.update_funding_rate("bybit", 0.0001);
    auto kill2 = enforcer.check_spread_collapse();
    ASSERT_TRUE(kill2.has_value());
    EXPECT_EQ(kill2->reason, KillReason::SPREAD_COLLAPSE);
}

TEST_F(DeltaNeutralTest, KillCondition_DrawdownLimit) {
    DeltaNeutralConfig config;
    config.max_session_drawdown = 0.03;  // 3%
    DeltaNeutralEnforcer enforcer(db_, session_id_, config);

    // Start with $10000
    enforcer.update_equity(10000);

    // Equity drops to $9500 (5% drawdown)
    enforcer.update_equity(9500);

    auto kill = enforcer.check_drawdown_limit();
    ASSERT_TRUE(kill.has_value());
    EXPECT_EQ(kill->reason, KillReason::DRAWDOWN_LIMIT);
}

TEST_F(DeltaNeutralTest, KillCondition_ExchangeHalt) {
    DeltaNeutralConfig config;
    config.exchange_timeout_ms = 100;  // Very short for test
    DeltaNeutralEnforcer enforcer(db_, session_id_, config);

    enforcer.record_heartbeat("binance");
    enforcer.record_heartbeat("bybit");

    // Immediate check - should be healthy
    auto kill1 = enforcer.check_exchange_halt();
    EXPECT_FALSE(kill1.has_value());

    // Wait for timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    auto kill2 = enforcer.check_exchange_halt();
    ASSERT_TRUE(kill2.has_value());
    EXPECT_EQ(kill2->reason, KillReason::EXCHANGE_HALT);
}

TEST_F(DeltaNeutralTest, GenerateKillOrders) {
    DeltaNeutralEnforcer enforcer(db_, session_id_);

    Position long_pos;
    long_pos.venue = "binance";
    long_pos.instrument = "BTCUSDT";
    long_pos.qty = 0.1;

    Position short_pos;
    short_pos.venue = "bybit";
    short_pos.instrument = "BTCUSDT";
    short_pos.qty = -0.1;

    enforcer.update_position(long_pos);
    enforcer.update_position(short_pos);

    auto orders = enforcer.generate_kill_orders();

    EXPECT_EQ(orders.size(), 2);

    // Should have one sell (close long) and one buy (close short)
    int sells = 0, buys = 0;
    for (const auto& o : orders) {
        if (o.side == OrderSide::SELL) sells++;
        if (o.side == OrderSide::BUY) buys++;
        EXPECT_EQ(o.urgency, 1.0);
    }
    EXPECT_EQ(sells, 1);
    EXPECT_EQ(buys, 1);
}

// ============================================================================
// Demo Harness Safety Tests
// ============================================================================

TEST_F(FundingSettlementTest, DemoHarness_SafetyLock) {
    DemoHarnessConfig config;
    config.live_mode = true;  // Try to enable live mode
    config.db_path = test_db_path_;

    // Should throw due to LIVE_ORDER_BLOCK
    EXPECT_THROW({
        DemoTradingHarness harness(config);
    }, std::runtime_error);
}

TEST_F(FundingSettlementTest, DemoHarness_CreateSession) {
    DemoHarnessConfig config;
    config.live_mode = false;
    config.starting_balance = 10000;
    config.session_name = "Test Session";
    config.db_path = test_db_path_;

    DemoTradingHarness harness(config);

    EXPECT_TRUE(harness.is_demo_mode());
    EXPECT_FALSE(harness.session_id().empty());
    EXPECT_NEAR(harness.get_equity(), 10000, 1);
}

// ============================================================================
// Integration Test
// ============================================================================

TEST_F(FundingSettlementTest, Integration_FundingDispersionCycle) {
    // This test simulates a complete funding dispersion cycle:
    // 1. Entry: short binance (high funding), long bybit (low funding)
    // 2. Multiple funding settlements
    // 3. Exit when spread compresses

    DemoFillConfig fill_config;
    fill_config.base_fill_probability = 1.0;  // Always fill for test

    FundingSettlementEngine settlement(db_, session_id_, fill_config);

    // Set up funding rates
    FundingRate binance_rate;
    binance_rate.venue = "binance";
    binance_rate.instrument = "BTCUSDT";
    binance_rate.rate = 0.0003;  // 0.03% - high
    binance_rate.mark_price = 45000;

    FundingRate bybit_rate;
    bybit_rate.venue = "bybit";
    bybit_rate.instrument = "BTCUSDT";
    bybit_rate.rate = 0.0001;  // 0.01% - low
    bybit_rate.mark_price = 45000;

    settlement.update_funding_rate(binance_rate);
    settlement.update_funding_rate(bybit_rate);

    // Positions (delta neutral)
    std::vector<Position> positions;
    Position short_pos, long_pos;

    short_pos.session_id = session_id_;
    short_pos.venue = "binance";
    short_pos.instrument = "BTCUSDT";
    short_pos.qty = -0.1;
    short_pos.avg_price = 45000;
    positions.push_back(short_pos);

    long_pos.session_id = session_id_;
    long_pos.venue = "bybit";
    long_pos.instrument = "BTCUSDT";
    long_pos.qty = 0.1;
    long_pos.avg_price = 45000;
    positions.push_back(long_pos);

    // Simulate 3 funding periods
    double total_funding = 0;
    for (int period = 0; period < 3; period++) {
        // Settle binance short
        double binance_payment = settlement.settle_funding(
            "binance", "BTCUSDT", -0.1, 45000);
        // Short receives: 0.1 * 45000 * 0.0003 = 1.35
        EXPECT_NEAR(binance_payment, 1.35, 0.01);

        // Settle bybit long
        double bybit_payment = settlement.settle_funding(
            "bybit", "BTCUSDT", 0.1, 45000);
        // Long pays: -0.1 * 45000 * 0.0001 = -0.45
        EXPECT_NEAR(bybit_payment, -0.45, 0.01);

        total_funding += binance_payment + bybit_payment;
    }

    // Expected: 3 * (1.35 - 0.45) = 3 * 0.90 = 2.70
    EXPECT_NEAR(total_funding, 2.70, 0.01);

    // Verify stats
    auto stats = settlement.stats();
    EXPECT_EQ(stats.funding_settlements, 6);  // 2 per period * 3 periods
    EXPECT_NEAR(stats.net_funding, 2.70, 0.01);

    // Verify database records
    auto events = db_->get_funding_events_for_session(session_id_);
    EXPECT_EQ(events.size(), 6);

    double db_total = db_->get_total_funding_for_session(session_id_);
    EXPECT_NEAR(db_total, 2.70, 0.01);
}
