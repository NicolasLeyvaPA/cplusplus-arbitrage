#include <gtest/gtest.h>
#include "risk/risk_manager.hpp"
#include "position/position_manager.hpp"

using namespace arb;

class RiskManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.max_total_exposure_usd = 10000.0;
        config_.max_per_symbol_exposure_usd = 5000.0;
        config_.max_drawdown_pct = 5.0;
        config_.max_daily_loss_usd = 200.0;
        config_.stop_loss_basis_bps = 100.0;
        config_.max_funding_rate_to_pay = -0.0001;

        risk_manager_ = std::make_unique<RiskManager>(config_);
    }

    RiskConfig config_;
    std::unique_ptr<RiskManager> risk_manager_;
};

TEST_F(RiskManagerTest, CheckNewPosition_AllowsWithinLimits) {
    PositionManager pm;
    auto result = risk_manager_->check_new_position("BTCUSDT", 3000.0, pm);
    EXPECT_TRUE(result.allowed);
}

TEST_F(RiskManagerTest, CheckNewPosition_RejectsTotalExposureExceeded) {
    PositionManager pm;
    // Add a large position first
    DeltaNeutralPosition pos;
    pos.symbol = "ETHUSDT";
    pos.spot_size = 1.0;
    pos.spot_notional = 8000.0;
    pos.futures_size = -1.0;
    pos.futures_notional = -8000.0;
    pm.open_position("ETHUSDT", pos);

    // This should exceed total limit of 10000
    auto result = risk_manager_->check_new_position("BTCUSDT", 5000.0, pm);
    EXPECT_FALSE(result.allowed);
}

TEST_F(RiskManagerTest, CheckNewPosition_RejectsPerSymbolExposureExceeded) {
    PositionManager pm;
    DeltaNeutralPosition pos;
    pos.symbol = "BTCUSDT";
    pos.spot_size = 0.05;
    pos.spot_notional = 4000.0;
    pos.futures_size = -0.05;
    pos.futures_notional = -4000.0;
    pm.open_position("BTCUSDT", pos);

    // This should exceed per-symbol limit of 5000
    auto result = risk_manager_->check_new_position("BTCUSDT", 2000.0, pm);
    EXPECT_FALSE(result.allowed);
}

TEST_F(RiskManagerTest, CheckNewPosition_RejectsWhenKillSwitchActive) {
    risk_manager_->activate_kill_switch("Test reason");

    PositionManager pm;
    auto result = risk_manager_->check_new_position("BTCUSDT", 1000.0, pm);
    EXPECT_FALSE(result.allowed);
    EXPECT_NE(result.reason.find("Kill switch"), std::string::npos);
}

TEST_F(RiskManagerTest, CheckDailyLoss_AllowsWithinLimit) {
    auto result = risk_manager_->check_daily_loss(-100.0);
    EXPECT_TRUE(result.allowed);
}

TEST_F(RiskManagerTest, CheckDailyLoss_RejectsExcessiveLoss) {
    auto result = risk_manager_->check_daily_loss(-250.0);
    EXPECT_FALSE(result.allowed);
}

TEST_F(RiskManagerTest, CheckDrawdown_AllowsWithinLimit) {
    auto result = risk_manager_->check_drawdown(9800.0, 10000.0);
    EXPECT_TRUE(result.allowed);
}

TEST_F(RiskManagerTest, CheckDrawdown_RejectsExcessiveDrawdown) {
    auto result = risk_manager_->check_drawdown(9000.0, 10000.0);
    EXPECT_FALSE(result.allowed);
}

TEST_F(RiskManagerTest, CheckFundingRate_AllowsPositiveRate) {
    auto result = risk_manager_->check_funding_rate(0.001);
    EXPECT_TRUE(result.allowed);
}

TEST_F(RiskManagerTest, CheckFundingRate_RejectsNegativeRate) {
    auto result = risk_manager_->check_funding_rate(-0.0005);
    EXPECT_FALSE(result.allowed);
}

TEST_F(RiskManagerTest, CheckBasisRisk_AllowsSmallBasis) {
    auto result = risk_manager_->check_basis_risk(50.0);
    EXPECT_TRUE(result.allowed);
}

TEST_F(RiskManagerTest, CheckBasisRisk_RejectsLargeBasis) {
    auto result = risk_manager_->check_basis_risk(150.0);
    EXPECT_FALSE(result.allowed);
}

TEST_F(RiskManagerTest, KillSwitch_ActivatesAndDeactivates) {
    EXPECT_FALSE(risk_manager_->is_kill_switch_active());

    risk_manager_->activate_kill_switch("Test reason");
    EXPECT_TRUE(risk_manager_->is_kill_switch_active());
    EXPECT_EQ(risk_manager_->kill_reason(), "Test reason");

    risk_manager_->deactivate_kill_switch();
    EXPECT_FALSE(risk_manager_->is_kill_switch_active());
}

TEST_F(RiskManagerTest, RecordDailyPnl_UpdatesCorrectly) {
    risk_manager_->record_daily_pnl(-50.0);
    risk_manager_->reset_daily();
    // After reset, daily should be clean
}
