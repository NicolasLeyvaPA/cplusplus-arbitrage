#include <gtest/gtest.h>
#include "strategy/basis_calculator.hpp"

using namespace arb;

TEST(BasisCalculatorTest, UpdateAndGet) {
    BasisCalculator calc;
    calc.update("BTCUSDT", 50000.0, 50100.0);

    auto basis = calc.get_basis("BTCUSDT");
    EXPECT_EQ(basis.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(basis.spot_mid, 50000.0);
    EXPECT_DOUBLE_EQ(basis.futures_mid, 50100.0);
    EXPECT_NEAR(basis.basis, 100.0 / 50000.0, 1e-10);
    EXPECT_NEAR(basis.basis_bps, 100.0 / 50000.0 * 10000.0, 0.01);
}

TEST(BasisCalculatorTest, NegativeBasis) {
    BasisCalculator calc;
    calc.update("ETHUSDT", 3000.0, 2990.0);

    auto basis = calc.get_basis("ETHUSDT");
    EXPECT_LT(basis.basis, 0.0);
    EXPECT_LT(basis.basis_bps, 0.0);
}

TEST(BasisCalculatorTest, BasisIsAcceptable) {
    BasisCalculator calc;
    calc.update("BTCUSDT", 50000.0, 50010.0);  // Very small basis

    EXPECT_TRUE(calc.basis_is_acceptable("BTCUSDT", 0.5));
}

TEST(BasisCalculatorTest, BasisNotAcceptable) {
    BasisCalculator calc;
    calc.update("BTCUSDT", 50000.0, 51000.0);  // 2% basis

    EXPECT_FALSE(calc.basis_is_acceptable("BTCUSDT", 0.5));
}

TEST(BasisCalculatorTest, GetAllBasis) {
    BasisCalculator calc;
    calc.update("BTCUSDT", 50000.0, 50100.0);
    calc.update("ETHUSDT", 3000.0, 3005.0);

    auto all = calc.get_all_basis();
    EXPECT_EQ(all.size(), 2);
    EXPECT_TRUE(all.count("BTCUSDT") > 0);
    EXPECT_TRUE(all.count("ETHUSDT") > 0);
}

TEST(BasisCalculatorTest, UnknownSymbol) {
    BasisCalculator calc;
    auto basis = calc.get_basis("UNKNOWN");
    EXPECT_EQ(basis.symbol, "");
    EXPECT_DOUBLE_EQ(basis.spot_mid, 0.0);
}
