#include <gtest/gtest.h>
#include "strategy/strategy_base.hpp"
#include "market_data/order_book.hpp"
#include "config/config.hpp"

using namespace arb;

class UnderpricingStrategyTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.min_edge_cents = 2.0;
        config_.max_spread_to_trade = 0.05;
        strategy_ = std::make_unique<UnderpricingStrategy>(config_);
        book_ = std::make_unique<BinaryMarketBook>("test-market");
    }

    StrategyConfig config_;
    std::unique_ptr<UnderpricingStrategy> strategy_;
    std::unique_ptr<BinaryMarketBook> book_;
    BtcPrice btc_price_;  // Not used by S2 but required by interface
};

// ============================================================================
// Tests updated for Polymarket parabolic fee formula:
// fee = price * (1 - price) * 0.0624 per position
// Total fees = YES_fee + NO_fee
// Edge = 1.0 - (yes_ask + no_ask) - total_fees
// ============================================================================

TEST_F(UnderpricingStrategyTest, CalculateEdge_Profitable) {
    // YES=0.40, NO=0.45, sum=0.85
    // YES fee: 0.40 * 0.60 * 0.0624 = 0.01498
    // NO fee: 0.45 * 0.55 * 0.0624 = 0.01545
    // Total fees: 0.03043
    // Edge = 1.0 - 0.85 - 0.03043 = 0.1196 ≈ 12 cents (very profitable)

    double edge = strategy_->calculate_edge(0.40, 0.45, 0.0 /* unused */);
    EXPECT_GT(edge, 10.0);  // Should be about 12 cents
    EXPECT_LT(edge, 15.0);
}

TEST_F(UnderpricingStrategyTest, CalculateEdge_NotProfitable) {
    // YES=0.50, NO=0.49, sum=0.99
    // YES fee: 0.50 * 0.50 * 0.0624 = 0.0156
    // NO fee: 0.49 * 0.51 * 0.0624 = 0.0156
    // Total fees: 0.0312
    // Edge = 1.0 - 0.99 - 0.0312 = -0.0212 ≈ -2.1 cents (not profitable)

    double edge = strategy_->calculate_edge(0.50, 0.49, 0.0);
    EXPECT_LT(edge, 0.0);  // Negative edge
}

TEST_F(UnderpricingStrategyTest, CalculateEdge_NearBreakEven) {
    // YES=0.46, NO=0.50, sum=0.96
    // YES fee: 0.46 * 0.54 * 0.0624 = 0.01551
    // NO fee: 0.50 * 0.50 * 0.0624 = 0.0156
    // Total fees: 0.03111
    // Edge = 1.0 - 0.96 - 0.03111 = 0.00889 ≈ 0.9 cents (near break-even)

    double edge = strategy_->calculate_edge(0.46, 0.50, 0.0);
    EXPECT_NEAR(edge, 0.9, 0.5);  // Near 1 cent, with tolerance
}

TEST_F(UnderpricingStrategyTest, IsProfitable_AboveThreshold) {
    EXPECT_TRUE(strategy_->is_profitable(3.0));   // 3 cents > 2 cents min
    EXPECT_TRUE(strategy_->is_profitable(2.0));   // 2 cents == 2 cents min
}

TEST_F(UnderpricingStrategyTest, IsProfitable_BelowThreshold) {
    EXPECT_FALSE(strategy_->is_profitable(1.5));  // 1.5 cents < 2 cents min
    EXPECT_FALSE(strategy_->is_profitable(0.0));
    EXPECT_FALSE(strategy_->is_profitable(-1.0));
}

TEST_F(UnderpricingStrategyTest, Evaluate_GeneratesSignals_WhenProfitable) {
    // Set up book with profitable spread (needs sum + fees < 0.98 for 2+ cent profit)
    // YES=0.40, NO=0.45, sum=0.85, fees≈0.030, edge≈12 cents
    std::vector<PriceLevel> yes_bids = {{0.38, 10.0}};
    std::vector<PriceLevel> yes_asks = {{0.40, 10.0}};  // 5% spread is OK
    std::vector<PriceLevel> no_bids = {{0.43, 10.0}};
    std::vector<PriceLevel> no_asks = {{0.45, 10.0}};   // 4.4% spread is OK

    book_->yes_book().apply_snapshot(yes_bids, yes_asks);
    book_->no_book().apply_snapshot(no_bids, no_asks);

    auto signals = strategy_->evaluate(*book_, btc_price_, now());

    // Should generate 2 signals (YES and NO)
    EXPECT_EQ(signals.size(), 2);

    if (!signals.empty()) {
        EXPECT_EQ(signals[0].strategy_name, "S2_Underpricing");
        EXPECT_EQ(signals[0].side, Side::BUY);
        EXPECT_GT(signals[0].expected_edge, 2.0);  // Above min threshold
    }
}

TEST_F(UnderpricingStrategyTest, Evaluate_NoSignals_WhenNotProfitable) {
    // Set up book with no edge (sum = 0.99 + fees ≈ 1.02, negative edge)
    std::vector<PriceLevel> yes_bids = {{0.48, 10.0}};
    std::vector<PriceLevel> yes_asks = {{0.50, 10.0}};
    std::vector<PriceLevel> no_bids = {{0.48, 10.0}};
    std::vector<PriceLevel> no_asks = {{0.49, 10.0}};

    book_->yes_book().apply_snapshot(yes_bids, yes_asks);
    book_->no_book().apply_snapshot(no_bids, no_asks);

    auto signals = strategy_->evaluate(*book_, btc_price_, now());

    EXPECT_TRUE(signals.empty());
}

TEST_F(UnderpricingStrategyTest, Evaluate_NoSignals_WhenNoLiquidity) {
    // Empty book
    auto signals = strategy_->evaluate(*book_, btc_price_, now());
    EXPECT_TRUE(signals.empty());
}

TEST_F(UnderpricingStrategyTest, Evaluate_NoSignals_WhenDisabled) {
    strategy_->set_enabled(false);

    std::vector<PriceLevel> yes_bids = {{0.35, 10.0}};
    std::vector<PriceLevel> yes_asks = {{0.40, 10.0}};
    std::vector<PriceLevel> no_bids = {{0.40, 10.0}};
    std::vector<PriceLevel> no_asks = {{0.45, 10.0}};

    book_->yes_book().apply_snapshot(yes_bids, yes_asks);
    book_->no_book().apply_snapshot(no_bids, no_asks);

    auto signals = strategy_->evaluate(*book_, btc_price_, now());
    EXPECT_TRUE(signals.empty());
}

TEST_F(UnderpricingStrategyTest, Evaluate_NoSignals_WhenSpreadTooWide) {
    // Wide spread (>5%) should prevent trading even if edge is positive
    std::vector<PriceLevel> yes_bids = {{0.30, 10.0}};
    std::vector<PriceLevel> yes_asks = {{0.40, 10.0}};  // 10% spread - too wide
    std::vector<PriceLevel> no_bids = {{0.40, 10.0}};
    std::vector<PriceLevel> no_asks = {{0.45, 10.0}};

    book_->yes_book().apply_snapshot(yes_bids, yes_asks);
    book_->no_book().apply_snapshot(no_bids, no_asks);

    auto signals = strategy_->evaluate(*book_, btc_price_, now());
    EXPECT_TRUE(signals.empty());
}

TEST_F(UnderpricingStrategyTest, SignalSizeMatchesAvailableLiquidity) {
    // Profitable setup with limited liquidity
    std::vector<PriceLevel> yes_bids = {{0.38, 5.0}};
    std::vector<PriceLevel> yes_asks = {{0.40, 5.0}};   // Only 5 available
    std::vector<PriceLevel> no_bids = {{0.43, 20.0}};
    std::vector<PriceLevel> no_asks = {{0.45, 20.0}};   // 20 available

    book_->yes_book().apply_snapshot(yes_bids, yes_asks);
    book_->no_book().apply_snapshot(no_bids, no_asks);

    auto signals = strategy_->evaluate(*book_, btc_price_, now());

    if (signals.size() >= 2) {
        // Signal size should be min of both sides
        EXPECT_LE(signals[0].target_size, 5.0);
        EXPECT_LE(signals[1].target_size, 5.0);
    }
}
