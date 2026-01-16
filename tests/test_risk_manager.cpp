#include <gtest/gtest.h>
#include "risk/risk_manager.hpp"

using namespace arb;

class RiskManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.max_notional_per_trade = 2.0;
        config_.max_daily_loss = 5.0;
        config_.max_open_positions = 3;
        config_.max_exposure_per_market = 3.0;
        config_.max_orders_per_minute = 10;

        risk_manager_ = std::make_unique<RiskManager>(config_, 50.0);
    }

    RiskConfig config_;
    std::unique_ptr<RiskManager> risk_manager_;

    Signal create_signal(const std::string& market_id = "test-market") {
        Signal signal;
        signal.market_id = market_id;
        signal.token_id = "test-token";
        signal.side = Side::BUY;
        signal.target_price = 0.50;
        signal.target_size = 2.0;
        return signal;
    }
};

TEST_F(RiskManagerTest, CheckOrder_AllowsValidOrder) {
    auto signal = create_signal();
    auto result = risk_manager_->check_order(signal, 1.0);

    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.reason.empty());
}

TEST_F(RiskManagerTest, CheckOrder_RejectsOverNotionalLimit) {
    auto signal = create_signal();
    auto result = risk_manager_->check_order(signal, 3.0);  // > 2.0 max

    EXPECT_FALSE(result.allowed);
    EXPECT_FALSE(result.reason.empty());
}

TEST_F(RiskManagerTest, CheckOrder_RejectsWhenKillSwitchActive) {
    risk_manager_->activate_kill_switch("Test reason");

    auto signal = create_signal();
    auto result = risk_manager_->check_order(signal, 1.0);

    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("Kill switch"), std::string::npos);
}

TEST_F(RiskManagerTest, CheckOrder_RejectsAfterDailyLossLimit) {
    // Record losses exceeding daily limit
    risk_manager_->record_pnl(-6.0);  // > 5.0 max daily loss

    auto signal = create_signal();
    auto result = risk_manager_->check_order(signal, 1.0);

    EXPECT_FALSE(result.allowed);
}

TEST_F(RiskManagerTest, CheckPositionLimit_AllowsWithinLimit) {
    auto result = risk_manager_->check_position_limit("market-1");
    EXPECT_TRUE(result.allowed);
}

TEST_F(RiskManagerTest, CheckPositionLimit_RejectsOverMaxPositions) {
    // Fill up positions
    Fill fill1, fill2, fill3;
    fill1.market_id = "market-1";
    fill1.side = Side::BUY;
    fill1.size = 1.0;
    fill1.price = 0.50;

    fill2.market_id = "market-2";
    fill2.side = Side::BUY;
    fill2.size = 1.0;
    fill2.price = 0.50;

    fill3.market_id = "market-3";
    fill3.side = Side::BUY;
    fill3.size = 1.0;
    fill3.price = 0.50;

    risk_manager_->record_fill(fill1);
    risk_manager_->record_fill(fill2);
    risk_manager_->record_fill(fill3);

    // Now at max positions
    auto result = risk_manager_->check_position_limit("market-4");
    EXPECT_FALSE(result.allowed);
}

TEST_F(RiskManagerTest, DailyPnL_TracksPnLCorrectly) {
    EXPECT_DOUBLE_EQ(risk_manager_->daily_pnl(), 0.0);

    risk_manager_->record_pnl(1.0);
    EXPECT_DOUBLE_EQ(risk_manager_->daily_pnl(), 1.0);

    risk_manager_->record_pnl(-0.5);
    EXPECT_DOUBLE_EQ(risk_manager_->daily_pnl(), 0.5);
}

TEST_F(RiskManagerTest, DailyLossRemaining_CalculatesCorrectly) {
    // Starting: max_daily_loss = 5.0, pnl = 0
    EXPECT_DOUBLE_EQ(risk_manager_->daily_loss_remaining(), 5.0);

    risk_manager_->record_pnl(-2.0);
    EXPECT_DOUBLE_EQ(risk_manager_->daily_loss_remaining(), 3.0);

    risk_manager_->record_pnl(-3.0);
    EXPECT_DOUBLE_EQ(risk_manager_->daily_loss_remaining(), 0.0);
}

TEST_F(RiskManagerTest, KillSwitch_ActivatesAndDeactivates) {
    EXPECT_FALSE(risk_manager_->is_kill_switch_active());

    risk_manager_->activate_kill_switch("Test reason");
    EXPECT_TRUE(risk_manager_->is_kill_switch_active());
    EXPECT_EQ(risk_manager_->kill_switch_reason(), "Test reason");

    risk_manager_->deactivate_kill_switch();
    EXPECT_FALSE(risk_manager_->is_kill_switch_active());
}

TEST_F(RiskManagerTest, KillSwitch_ActivatesOnStopLoss) {
    // Stop loss threshold is 10% of 50 = 5.0
    // Recording -10 loss should trigger stop loss (20% loss)
    risk_manager_->record_pnl(-10.0);

    EXPECT_TRUE(risk_manager_->is_kill_switch_active());
}

TEST_F(RiskManagerTest, RateLimit_AllowsWithinLimit) {
    for (int i = 0; i < 10; i++) {
        EXPECT_TRUE(risk_manager_->can_place_order());
        risk_manager_->record_order_placed();
    }
}

TEST_F(RiskManagerTest, RateLimit_BlocksOverLimit) {
    // Place 10 orders (the limit)
    for (int i = 0; i < 10; i++) {
        risk_manager_->record_order_placed();
    }

    // 11th should be blocked
    EXPECT_FALSE(risk_manager_->can_place_order());
}

TEST_F(RiskManagerTest, AvailableBalance_CalculatesCorrectly) {
    // Starting balance: 50.0, no exposure
    EXPECT_DOUBLE_EQ(risk_manager_->available_balance(), 50.0);

    // Add some exposure
    Fill fill;
    fill.market_id = "market-1";
    fill.side = Side::BUY;
    fill.size = 10.0;
    fill.price = 0.50;  // 5.0 notional
    risk_manager_->record_fill(fill);

    // Available = 50 - 5 = 45
    EXPECT_DOUBLE_EQ(risk_manager_->available_balance(), 45.0);
}

TEST_F(RiskManagerTest, ResetDailyCounters_ResetsCorrectly) {
    risk_manager_->record_pnl(-2.0);
    EXPECT_DOUBLE_EQ(risk_manager_->daily_pnl(), -2.0);

    risk_manager_->reset_daily_counters();
    EXPECT_DOUBLE_EQ(risk_manager_->daily_pnl(), 0.0);
}

TEST_F(RiskManagerTest, SlippageTracking_ActivatesKillSwitchOnExcessive) {
    // Record multiple high slippage events
    for (int i = 0; i < 6; i++) {
        risk_manager_->record_slippage(100.0);  // > 50 threshold
    }

    EXPECT_TRUE(risk_manager_->is_kill_switch_active());
}

TEST_F(RiskManagerTest, ConnectivityTracking_ActivatesKillSwitchOnIssues) {
    // Record multiple connectivity issues
    for (int i = 0; i < 10; i++) {
        risk_manager_->record_connectivity_issue();
    }

    EXPECT_TRUE(risk_manager_->is_kill_switch_active());
}
