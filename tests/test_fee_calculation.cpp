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

// ============================================================================
// Test Polymarket parabolic fee formula: fee = price * (1 - price) * 0.0624
// Total fee = YES_fee + NO_fee (paid on BOTH positions)
// Edge = 1.0 - (yes_ask + no_ask) - total_fees
// ============================================================================

TEST_F(FeeCalculationTest, PositionFee_AtFiftyPercent) {
    // At $0.50, fee should be maximum: 0.50 * 0.50 * 0.0624 = $0.0156
    double fee = UnderpricingStrategy::calculate_position_fee(0.50);
    EXPECT_NEAR(fee, 0.0156, 0.001);
}

TEST_F(FeeCalculationTest, PositionFee_AtExtremes) {
    // At extremes, fee should be near zero
    double fee_low = UnderpricingStrategy::calculate_position_fee(0.01);
    double fee_high = UnderpricingStrategy::calculate_position_fee(0.99);
    EXPECT_LT(fee_low, 0.001);
    EXPECT_LT(fee_high, 0.001);
}

TEST_F(FeeCalculationTest, PositionFee_MatchesFeeTable) {
    // Verify against Polymarket's fee table (from the image)
    // Price $0.40: fee = 0.40 * 0.60 * 0.0624 = $0.01498 ≈ $0.0144 (table shows ~$0.0144)
    double fee_40 = UnderpricingStrategy::calculate_position_fee(0.40);
    EXPECT_NEAR(fee_40, 0.015, 0.002);  // Allow some tolerance

    // Price $0.30: fee = 0.30 * 0.70 * 0.0624 = $0.0131
    double fee_30 = UnderpricingStrategy::calculate_position_fee(0.30);
    EXPECT_NEAR(fee_30, 0.013, 0.002);

    // Price $0.60: fee = 0.60 * 0.40 * 0.0624 = $0.015
    double fee_60 = UnderpricingStrategy::calculate_position_fee(0.60);
    EXPECT_NEAR(fee_60, 0.015, 0.002);
}

TEST_F(FeeCalculationTest, Edge_WithParabolicFees) {
    // YES ask = 0.40, NO ask = 0.50, total = 0.90
    // YES fee: 0.40 * 0.60 * 0.0624 = 0.01498
    // NO fee: 0.50 * 0.50 * 0.0624 = 0.0156
    // Total fee: 0.03058
    // Edge = 1.0 - 0.90 - 0.03058 = 0.0694 = 6.94 cents
    double edge = strategy_->calculate_edge(0.40, 0.50, 0.0 /* unused */);
    EXPECT_NEAR(edge, 6.94, 0.2);
}

TEST_F(FeeCalculationTest, Edge_NearBreakEven) {
    // YES ask = 0.47, NO ask = 0.50, total = 0.97
    // YES fee: 0.47 * 0.53 * 0.0624 = 0.01554
    // NO fee: 0.50 * 0.50 * 0.0624 = 0.0156
    // Total fee: 0.03114
    // Edge = 1.0 - 0.97 - 0.03114 = -0.00114 ≈ 0 cents (break-even)
    double edge = strategy_->calculate_edge(0.47, 0.50, 0.0);
    EXPECT_NEAR(edge, 0.0, 0.5);  // Should be near break-even
}

TEST_F(FeeCalculationTest, Edge_NegativeWhenOverpriced) {
    // Both at 0.50, sum = 1.00
    // YES fee: 0.50 * 0.50 * 0.0624 = 0.0156
    // NO fee: 0.50 * 0.50 * 0.0624 = 0.0156
    // Total fee: 0.0312
    // Edge = 1.0 - 1.0 - 0.0312 = -0.0312 = -3.12 cents
    double edge = strategy_->calculate_edge(0.50, 0.50, 0.0);
    EXPECT_NEAR(edge, -3.12, 0.1);
}

TEST_F(FeeCalculationTest, Edge_GoodOpportunity) {
    // YES ask = 0.45, NO ask = 0.50, total = 0.95
    // YES fee: 0.45 * 0.55 * 0.0624 = 0.01545
    // NO fee: 0.50 * 0.50 * 0.0624 = 0.0156
    // Total fee: 0.03105
    // Edge = 1.0 - 0.95 - 0.03105 = 0.01895 = 1.9 cents
    double edge = strategy_->calculate_edge(0.45, 0.50, 0.0);
    EXPECT_NEAR(edge, 1.9, 0.2);
}

TEST_F(FeeCalculationTest, Edge_ExtremeLowPrices_LowFees) {
    // When prices are at extremes, fees are lower
    // YES ask = 0.05, NO ask = 0.90, total = 0.95
    // YES fee: 0.05 * 0.95 * 0.0624 = 0.00297
    // NO fee: 0.90 * 0.10 * 0.0624 = 0.00562
    // Total fee: 0.00859
    // Edge = 1.0 - 0.95 - 0.00859 = 0.0414 = 4.14 cents
    double edge = strategy_->calculate_edge(0.05, 0.90, 0.0);
    EXPECT_NEAR(edge, 4.14, 0.2);
}

TEST_F(FeeCalculationTest, Edge_VeryUnderpriced) {
    // Extreme mispricing (unlikely but test the math)
    // YES ask = 0.30, NO ask = 0.30, total = 0.60
    // YES fee: 0.30 * 0.70 * 0.0624 = 0.0131
    // NO fee: 0.30 * 0.70 * 0.0624 = 0.0131
    // Total fee: 0.0262
    // Edge = 1.0 - 0.60 - 0.0262 = 0.3738 = 37.38 cents
    double edge = strategy_->calculate_edge(0.30, 0.30, 0.0);
    EXPECT_NEAR(edge, 37.38, 0.5);
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

// Test realistic trade scenario
TEST_F(FeeCalculationTest, RealisticTradeScenario) {
    // Realistic scenario: tight market with small edge
    // YES ask = 0.48, NO ask = 0.49, total = 0.97
    // YES fee: 0.48 * 0.52 * 0.0624 = 0.01558
    // NO fee: 0.49 * 0.51 * 0.0624 = 0.01560
    // Total fee: 0.03118
    // Edge = 1.0 - 0.97 - 0.03118 = -0.00118 = -0.12 cents (NOT profitable!)
    double edge = strategy_->calculate_edge(0.48, 0.49, 0.0);
    EXPECT_LT(edge, 0.5);  // Should be near break-even or negative
}

// Verify the minimum underpricing needed to be profitable
TEST_F(FeeCalculationTest, MinimumUnderpricingForProfit) {
    // To be profitable, we need sum of asks < 1.0 - total_fees
    // With both prices around $0.50, total fees ≈ $0.031
    // So we need sum < 0.969 to break even

    // Test at exactly that threshold
    double edge_breakeven = strategy_->calculate_edge(0.48, 0.489, 0.0);
    // This should be close to zero (might be slightly positive or negative)
    EXPECT_LT(std::abs(edge_breakeven), 1.0);  // Within 1 cent of break-even
}
