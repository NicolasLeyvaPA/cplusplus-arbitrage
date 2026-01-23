#include <gtest/gtest.h>
#include "arbitrage/funding_dispersion.hpp"

using namespace arb;

class FundingDispersionTest : public ::testing::Test {
protected:
    void SetUp() override {
        FundingDispersionStrategy::Config config;
        config.entry_spread_threshold = 0.0002;  // 0.02% = 22% annual
        config.exit_spread_threshold = 0.00005;
        strategy_ = std::make_unique<FundingDispersionStrategy>(config);
    }

    std::unique_ptr<FundingDispersionStrategy> strategy_;
};

// ============================================================================
// Mathematical Verification Tests
// ============================================================================

TEST_F(FundingDispersionTest, FundingSpread_AnnualizedCalculation) {
    // If spread = 0.02% per 8h period
    // Annualized = 0.02% * 3 * 365 = 21.9%
    FundingSpread spread;
    spread.short_funding = 0.0003;   // 0.03%
    spread.long_funding = 0.0001;    // 0.01%
    spread.spread = 0.0002;          // 0.02%

    double annual = spread.spread_annualized();
    EXPECT_NEAR(annual, 21.9, 0.5);  // ~22% annual
}

TEST_F(FundingDispersionTest, FundingRate_Annualized) {
    FundingRateSnapshot snapshot;
    snapshot.funding_rate = 0.0001;  // 0.01% per period

    // 0.01% * 3 periods/day * 365 days = 10.95%
    double annual = snapshot.annualized();
    EXPECT_NEAR(annual, 10.95, 0.5);
}

TEST_F(FundingDispersionTest, DetectsOpportunity_WhenSpreadAboveThreshold) {
    FundingRateSnapshot binance;
    binance.exchange = "binance";
    binance.symbol = "BTC";
    binance.funding_rate = 0.00035;  // 0.035%

    FundingRateSnapshot bybit;
    bybit.exchange = "bybit";
    bybit.symbol = "BTC";
    bybit.funding_rate = 0.0001;   // 0.01%

    strategy_->update_funding_rate(binance);
    strategy_->update_funding_rate(bybit);

    auto analysis = strategy_->analyze();

    // Spread = 0.035% - 0.01% = 0.025% > 0.02% threshold
    EXPECT_TRUE(analysis.should_enter);
    EXPECT_EQ(analysis.best_spread.short_exchange, "binance");  // Higher funding
    EXPECT_EQ(analysis.best_spread.long_exchange, "bybit");     // Lower funding
    EXPECT_NEAR(analysis.best_spread.spread, 0.00025, 0.00002);
}

TEST_F(FundingDispersionTest, NoOpportunity_WhenSpreadBelowThreshold) {
    FundingRateSnapshot binance;
    binance.exchange = "binance";
    binance.symbol = "BTC";
    binance.funding_rate = 0.00011;

    FundingRateSnapshot bybit;
    bybit.exchange = "bybit";
    bybit.symbol = "BTC";
    bybit.funding_rate = 0.0001;

    strategy_->update_funding_rate(binance);
    strategy_->update_funding_rate(bybit);

    auto analysis = strategy_->analyze();

    // Spread = 0.00001 < threshold
    EXPECT_FALSE(analysis.should_enter);
}

TEST_F(FundingDispersionTest, EntrySignal_CorrectExchanges) {
    FundingRateSnapshot okx;
    okx.exchange = "okx";
    okx.symbol = "BTC";
    okx.funding_rate = 0.0005;  // Highest - we short here

    FundingRateSnapshot dydx;
    dydx.exchange = "dydx";
    dydx.symbol = "BTC";
    dydx.funding_rate = -0.0001;  // Lowest/negative - we long here

    strategy_->update_funding_rate(okx);
    strategy_->update_funding_rate(dydx);

    auto signal = strategy_->evaluate_entry(10000);  // $10k capital

    EXPECT_TRUE(signal.should_enter);
    EXPECT_EQ(signal.short_exchange, "okx");
    EXPECT_EQ(signal.long_exchange, "dydx");

    // Spread = 0.05% - (-0.01%) = 0.06% per period
    // Annual (with 2x leverage) = 0.06% * 3 * 365 * 2 = 131.4%
    // (minus fees, so somewhat less)
    EXPECT_GT(signal.expected_annual_return, 100.0);
}

TEST_F(FundingDispersionTest, ExitSignal_WhenSpreadCompresses) {
    // First, set up a position scenario
    FundingPosition position;
    position.long_exchange = "bybit";
    position.short_exchange = "binance";
    position.long_entry_funding = 0.0001;
    position.short_entry_funding = 0.0003;

    // Now funding has compressed
    FundingRateSnapshot binance;
    binance.exchange = "binance";
    binance.symbol = "BTC";
    binance.funding_rate = 0.00011;

    FundingRateSnapshot bybit;
    bybit.exchange = "bybit";
    bybit.symbol = "BTC";
    bybit.funding_rate = 0.0001;

    strategy_->update_funding_rate(binance);
    strategy_->update_funding_rate(bybit);

    auto exit = strategy_->evaluate_exit(position);

    EXPECT_TRUE(exit.should_exit);
    EXPECT_TRUE(exit.reason.find("COMPRESSED") != std::string::npos);
}

TEST_F(FundingDispersionTest, ExitSignal_WhenBasisDiverges) {
    FundingPosition position;
    position.long_exchange = "bybit";
    position.short_exchange = "binance";
    position.current_basis_divergence = 0.01;  // 1% divergence > 0.5% limit

    auto exit = strategy_->evaluate_exit(position);

    EXPECT_TRUE(exit.should_exit);
    EXPECT_TRUE(exit.reason.find("BASIS") != std::string::npos);
    EXPECT_GT(exit.urgency, 0.8);  // High urgency
}

TEST_F(FundingDispersionTest, KellyPositionSize_Reasonable) {
    // Edge = 0.02%, Volatility = 0.02%
    // Kelly = edge / vol^2 = 0.0002 / 0.0002^2 = 5000 (100% of capital many times)
    // Half-Kelly = 2500, but capped by max_position_per_exchange
    double size = strategy_->kelly_position_size(10000, 0.0002, 0.0002);

    // Should be capped at reasonable level (25% of capital = $2500)
    EXPECT_LE(size, 2500);
    EXPECT_GT(size, 0);
}

TEST_F(FundingDispersionTest, RiskMetrics_SharpeEstimate) {
    // Add some funding rate history
    for (int i = 0; i < 100; i++) {
        FundingRateSnapshot binance;
        binance.exchange = "binance";
        binance.symbol = "BTC";
        binance.funding_rate = 0.0002 + (i % 10) * 0.00001;  // Varying around 0.02%

        FundingRateSnapshot bybit;
        bybit.exchange = "bybit";
        bybit.symbol = "BTC";
        bybit.funding_rate = 0.0001;

        strategy_->update_funding_rate(binance);
        strategy_->update_funding_rate(bybit);
        strategy_->analyze();  // Records history
    }

    auto metrics = strategy_->compute_risk_metrics();

    // Should have positive Sharpe
    EXPECT_GT(metrics.sharpe_estimate, 0);

    // VaR should be small (well under 1% daily)
    EXPECT_LT(metrics.var_99, 0.01);
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST_F(FundingDispersionTest, HandlesSingleExchange) {
    FundingRateSnapshot binance;
    binance.exchange = "binance";
    binance.symbol = "BTC";
    binance.funding_rate = 0.001;

    strategy_->update_funding_rate(binance);

    auto analysis = strategy_->analyze();

    // Can't compute spread with only one exchange
    EXPECT_FALSE(analysis.should_enter);
    EXPECT_EQ(analysis.best_spread.spread, 0);
}

TEST_F(FundingDispersionTest, HandlesNegativeFunding) {
    FundingRateSnapshot binance;
    binance.exchange = "binance";
    binance.symbol = "BTC";
    binance.funding_rate = -0.0001;  // Negative: shorts pay longs

    FundingRateSnapshot bybit;
    bybit.exchange = "bybit";
    bybit.symbol = "BTC";
    bybit.funding_rate = -0.0003;   // More negative

    strategy_->update_funding_rate(binance);
    strategy_->update_funding_rate(bybit);

    auto signal = strategy_->evaluate_entry(10000);

    // Short where funding is "highest" (least negative) = binance
    // Long where funding is "lowest" (most negative) = bybit
    if (signal.should_enter) {
        EXPECT_EQ(signal.short_exchange, "binance");
        EXPECT_EQ(signal.long_exchange, "bybit");
    }
}

TEST_F(FundingDispersionTest, Stats_TrackCorrectly) {
    FundingRateSnapshot binance;
    binance.exchange = "binance";
    binance.symbol = "BTC";
    binance.funding_rate = 0.0005;

    FundingRateSnapshot bybit;
    bybit.exchange = "bybit";
    bybit.symbol = "BTC";
    bybit.funding_rate = 0.0001;

    strategy_->update_funding_rate(binance);
    strategy_->update_funding_rate(bybit);

    auto stats1 = strategy_->stats();
    EXPECT_EQ(stats1.funding_updates, 2);

    strategy_->analyze();

    auto stats2 = strategy_->stats();
    EXPECT_EQ(stats2.opportunities_detected, 1);

    strategy_->evaluate_entry(10000);

    auto stats3 = strategy_->stats();
    EXPECT_EQ(stats3.entries_signaled, 1);
}

// ============================================================================
// Integration-style test: Full flow
// ============================================================================

TEST_F(FundingDispersionTest, FullFlow_EntryToExit) {
    // 1. Initial state: good opportunity
    FundingRateSnapshot binance;
    binance.exchange = "binance";
    binance.symbol = "BTC";
    binance.funding_rate = 0.0004;

    FundingRateSnapshot bybit;
    bybit.exchange = "bybit";
    bybit.symbol = "BTC";
    bybit.funding_rate = 0.0001;

    strategy_->update_funding_rate(binance);
    strategy_->update_funding_rate(bybit);

    // Should signal entry
    auto entry = strategy_->evaluate_entry(10000);
    EXPECT_TRUE(entry.should_enter);

    // 2. Simulate position
    FundingPosition position;
    position.short_exchange = entry.short_exchange;
    position.long_exchange = entry.long_exchange;
    position.short_entry_funding = 0.0004;
    position.long_entry_funding = 0.0001;

    // 3. Funding remains good - no exit
    auto exit1 = strategy_->evaluate_exit(position);
    EXPECT_FALSE(exit1.should_exit);

    // 4. Funding compresses - should exit
    FundingRateSnapshot binance2;
    binance2.exchange = "binance";
    binance2.symbol = "BTC";
    binance2.funding_rate = 0.00011;

    FundingRateSnapshot bybit2;
    bybit2.exchange = "bybit";
    bybit2.symbol = "BTC";
    bybit2.funding_rate = 0.0001;

    strategy_->update_funding_rate(binance2);
    strategy_->update_funding_rate(bybit2);

    auto exit2 = strategy_->evaluate_exit(position);
    EXPECT_TRUE(exit2.should_exit);
}
