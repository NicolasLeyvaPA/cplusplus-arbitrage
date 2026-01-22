#include <gtest/gtest.h>
#include "strategy/strategy_base.hpp"
#include "config/config.hpp"

using namespace arb;

class FeeCalculationTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.min_edge_cents = 0.0;  // Allow all edges for testing
        strategy_ = std::make_unique<UnderpricingStrategy>(config_);
    }

    StrategyConfig config_;
    std::unique_ptr<UnderpricingStrategy> strategy_;
};

// Test various fee scenarios
TEST_F(FeeCalculationTest, ZeroFees_FullEdge) {
    // With 0% fee
    // Buy YES at 0.40, NO at 0.50 = 0.90 total
    // Payout = 1.00
    // Edge = 1.00 - 0.90 = 0.10 = 10 cents
    double edge = strategy_->calculate_edge(0.40, 0.50, 0.0);
    EXPECT_NEAR(edge, 10.0, 0.01);
}

TEST_F(FeeCalculationTest, TwoPercentFee_ReducesEdge) {
    // With 2% fee (200 bps)
    // Buy YES at 0.40, NO at 0.50 = 0.90 total
    // Payout = 1.00
    // Fee = 0.02
    // Net payout = 0.98
    // Edge = 0.98 - 0.90 = 0.08 = 8 cents
    double edge = strategy_->calculate_edge(0.40, 0.50, 200.0);
    EXPECT_NEAR(edge, 8.0, 0.01);
}

TEST_F(FeeCalculationTest, HighFee_MayEliminateEdge) {
    // With 10% fee (1000 bps)
    // Buy YES at 0.45, NO at 0.50 = 0.95 total
    // Payout = 1.00
    // Fee = 0.10
    // Net payout = 0.90
    // Edge = 0.90 - 0.95 = -0.05 = -5 cents (negative!)
    double edge = strategy_->calculate_edge(0.45, 0.50, 1000.0);
    EXPECT_LT(edge, 0.0);
}

TEST_F(FeeCalculationTest, BreakEvenWithFees) {
    // Find break-even point with 2% fee
    // Net payout = 0.98
    // To break even, total cost must equal 0.98
    // If YES ask = 0.50, NO ask = 0.48, total = 0.98
    double edge = strategy_->calculate_edge(0.50, 0.48, 200.0);
    EXPECT_NEAR(edge, 0.0, 0.1);
}

TEST_F(FeeCalculationTest, SmallEdge_Marginal) {
    // Very small edge scenario
    // YES ask = 0.49, NO ask = 0.48, total = 0.97
    // With 2% fee, net payout = 0.98
    // Edge = 0.98 - 0.97 = 0.01 = 1 cent
    double edge = strategy_->calculate_edge(0.49, 0.48, 200.0);
    EXPECT_NEAR(edge, 1.0, 0.1);
}

TEST_F(FeeCalculationTest, ExtremeImbalance_HighYesLowNo) {
    // YES very expensive, NO very cheap
    // YES ask = 0.90, NO ask = 0.05, total = 0.95
    // With 2% fee, net payout = 0.98
    // Edge = 0.98 - 0.95 = 0.03 = 3 cents
    double edge = strategy_->calculate_edge(0.90, 0.05, 200.0);
    EXPECT_NEAR(edge, 3.0, 0.1);
}

TEST_F(FeeCalculationTest, EqualPrices_Sum100) {
    // Both at 0.50, sum = 1.00
    // With 2% fee, net payout = 0.98
    // Edge = 0.98 - 1.00 = -0.02 = -2 cents (negative)
    double edge = strategy_->calculate_edge(0.50, 0.50, 200.0);
    EXPECT_NEAR(edge, -2.0, 0.1);
}

TEST_F(FeeCalculationTest, VeryLowSum_HighEdge) {
    // Extreme mispricing
    // YES ask = 0.30, NO ask = 0.30, total = 0.60
    // With 2% fee, net payout = 0.98
    // Edge = 0.98 - 0.60 = 0.38 = 38 cents
    double edge = strategy_->calculate_edge(0.30, 0.30, 200.0);
    EXPECT_NEAR(edge, 38.0, 0.1);
}

// Test profitability threshold
TEST_F(FeeCalculationTest, Profitability_VariousThresholds) {
    StrategyConfig config;
    config.min_edge_cents = 1.0;
    UnderpricingStrategy strategy1(config);
    EXPECT_TRUE(strategy1.is_profitable(1.0));
    EXPECT_TRUE(strategy1.is_profitable(2.0));
    EXPECT_FALSE(strategy1.is_profitable(0.5));

    config.min_edge_cents = 5.0;
    UnderpricingStrategy strategy5(config);
    EXPECT_FALSE(strategy5.is_profitable(4.0));
    EXPECT_TRUE(strategy5.is_profitable(5.0));
    EXPECT_TRUE(strategy5.is_profitable(10.0));
}

// Edge case tests
TEST_F(FeeCalculationTest, EdgeCase_ZeroPrices) {
    // Edge case with zero prices
    double edge = strategy_->calculate_edge(0.0, 0.0, 200.0);
    // Should handle gracefully
    EXPECT_GT(edge, 0.0);  // Huge edge with zero cost
}

TEST_F(FeeCalculationTest, EdgeCase_OnePriceZero) {
    double edge = strategy_->calculate_edge(0.50, 0.0, 200.0);
    // Cost = 0.50, payout = 0.98, edge = 48 cents
    EXPECT_NEAR(edge, 48.0, 0.1);
}

TEST_F(FeeCalculationTest, EdgeCase_PricesNearOne) {
    // Both prices near 1.0 (overpriced market)
    double edge = strategy_->calculate_edge(0.95, 0.95, 200.0);
    // Cost = 1.90, payout = 0.98
    // Edge = 0.98 - 1.90 = -0.92 = -92 cents
    EXPECT_LT(edge, -90.0);
}
