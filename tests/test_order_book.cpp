#include <gtest/gtest.h>
#include "market_data/order_book.hpp"
#include <thread>

using namespace arb;

class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        book_ = std::make_unique<OrderBook>("TEST", 10);
    }

    std::unique_ptr<OrderBook> book_;
};

TEST_F(OrderBookTest, EmptyBook_ReturnsNullopt) {
    EXPECT_FALSE(book_->best_bid().has_value());
    EXPECT_FALSE(book_->best_ask().has_value());
    EXPECT_DOUBLE_EQ(book_->mid_price(), 0.0);
    EXPECT_DOUBLE_EQ(book_->spread(), 0.0);
}

TEST_F(OrderBookTest, UpdateBid_AddsLevel) {
    book_->update_bid(100.0, 5.0);

    auto best = book_->best_bid();
    ASSERT_TRUE(best.has_value());
    EXPECT_DOUBLE_EQ(best->price, 100.0);
    EXPECT_DOUBLE_EQ(best->size, 5.0);
}

TEST_F(OrderBookTest, UpdateAsk_AddsLevel) {
    book_->update_ask(101.0, 5.0);

    auto best = book_->best_ask();
    ASSERT_TRUE(best.has_value());
    EXPECT_DOUBLE_EQ(best->price, 101.0);
    EXPECT_DOUBLE_EQ(best->size, 5.0);
}

TEST_F(OrderBookTest, UpdateBid_ZeroSizeRemovesLevel) {
    book_->update_bid(100.0, 5.0);
    book_->update_bid(100.0, 0.0);

    EXPECT_FALSE(book_->best_bid().has_value());
}

TEST_F(OrderBookTest, MultipleBids_SortedDescending) {
    book_->update_bid(100.0, 1.0);
    book_->update_bid(102.0, 2.0);
    book_->update_bid(101.0, 3.0);

    auto best = book_->best_bid();
    ASSERT_TRUE(best.has_value());
    EXPECT_DOUBLE_EQ(best->price, 102.0);  // Highest bid first

    auto top = book_->top_bids(3);
    ASSERT_EQ(top.size(), 3);
    EXPECT_DOUBLE_EQ(top[0].price, 102.0);
    EXPECT_DOUBLE_EQ(top[1].price, 101.0);
    EXPECT_DOUBLE_EQ(top[2].price, 100.0);
}

TEST_F(OrderBookTest, MultipleAsks_SortedAscending) {
    book_->update_ask(103.0, 1.0);
    book_->update_ask(101.0, 2.0);
    book_->update_ask(102.0, 3.0);

    auto best = book_->best_ask();
    ASSERT_TRUE(best.has_value());
    EXPECT_DOUBLE_EQ(best->price, 101.0);  // Lowest ask first

    auto top = book_->top_asks(3);
    ASSERT_EQ(top.size(), 3);
    EXPECT_DOUBLE_EQ(top[0].price, 101.0);
    EXPECT_DOUBLE_EQ(top[1].price, 102.0);
    EXPECT_DOUBLE_EQ(top[2].price, 103.0);
}

TEST_F(OrderBookTest, MidPrice_Calculated) {
    book_->update_bid(100.0, 1.0);
    book_->update_ask(102.0, 1.0);

    EXPECT_DOUBLE_EQ(book_->mid_price(), 101.0);
}

TEST_F(OrderBookTest, Spread_Calculated) {
    book_->update_bid(100.0, 1.0);
    book_->update_ask(102.0, 1.0);

    EXPECT_DOUBLE_EQ(book_->spread(), 2.0);
}

TEST_F(OrderBookTest, SpreadBps_Calculated) {
    book_->update_bid(100.0, 1.0);
    book_->update_ask(102.0, 1.0);

    // spread = 2, mid = 101, spread_bps = 2/101 * 10000 ≈ 198
    double expected_bps = (2.0 / 101.0) * 10000.0;
    EXPECT_NEAR(book_->spread_bps(), expected_bps, 0.01);
}

TEST_F(OrderBookTest, ApplySnapshot_ReplacesBook) {
    book_->update_bid(100.0, 1.0);
    book_->update_ask(105.0, 1.0);

    std::vector<PriceLevel> new_bids = {{102.0, 5.0}, {101.0, 3.0}};
    std::vector<PriceLevel> new_asks = {{103.0, 4.0}, {104.0, 2.0}};

    book_->apply_snapshot(new_bids, new_asks);

    auto best_bid = book_->best_bid();
    auto best_ask = book_->best_ask();

    ASSERT_TRUE(best_bid.has_value());
    ASSERT_TRUE(best_ask.has_value());

    EXPECT_DOUBLE_EQ(best_bid->price, 102.0);
    EXPECT_DOUBLE_EQ(best_ask->price, 103.0);
}

TEST_F(OrderBookTest, BidDepth_SumsCorrectly) {
    book_->update_bid(100.0, 5.0);
    book_->update_bid(99.0, 3.0);
    book_->update_bid(98.0, 2.0);

    EXPECT_DOUBLE_EQ(book_->bid_depth(1), 5.0);
    EXPECT_DOUBLE_EQ(book_->bid_depth(2), 8.0);
    EXPECT_DOUBLE_EQ(book_->bid_depth(3), 10.0);
    EXPECT_DOUBLE_EQ(book_->bid_depth(10), 10.0);  // All levels
}

TEST_F(OrderBookTest, AskDepth_SumsCorrectly) {
    book_->update_ask(101.0, 4.0);
    book_->update_ask(102.0, 3.0);
    book_->update_ask(103.0, 1.0);

    EXPECT_DOUBLE_EQ(book_->ask_depth(1), 4.0);
    EXPECT_DOUBLE_EQ(book_->ask_depth(2), 7.0);
    EXPECT_DOUBLE_EQ(book_->ask_depth(3), 8.0);
}

TEST_F(OrderBookTest, Clear_EmptiesBook) {
    book_->update_bid(100.0, 5.0);
    book_->update_ask(101.0, 5.0);

    book_->clear();

    EXPECT_FALSE(book_->best_bid().has_value());
    EXPECT_FALSE(book_->best_ask().has_value());
}

TEST_F(OrderBookTest, Staleness_DetectedCorrectly) {
    book_->update_bid(100.0, 5.0);

    // Should not be stale immediately
    EXPECT_FALSE(book_->is_stale(std::chrono::seconds(1)));

    // Wait and check
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(book_->is_stale(std::chrono::milliseconds(50)));
}

TEST_F(OrderBookTest, MaxLevels_TrimsExcess) {
    auto small_book = std::make_unique<OrderBook>("SMALL", 3);

    small_book->update_bid(100.0, 1.0);
    small_book->update_bid(99.0, 1.0);
    small_book->update_bid(98.0, 1.0);
    small_book->update_bid(97.0, 1.0);  // Should be trimmed

    auto top = small_book->top_bids(10);
    EXPECT_EQ(top.size(), 3);  // Max 3 levels
}

// BinaryMarketBook Tests

class BinaryMarketBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        book_ = std::make_unique<BinaryMarketBook>("test-market");
    }

    std::unique_ptr<BinaryMarketBook> book_;
};

TEST_F(BinaryMarketBookTest, SumOfBestAsks_CalculatedCorrectly) {
    std::vector<PriceLevel> yes_bids = {{0.48, 10.0}};
    std::vector<PriceLevel> yes_asks = {{0.50, 10.0}};
    std::vector<PriceLevel> no_bids = {{0.45, 10.0}};
    std::vector<PriceLevel> no_asks = {{0.48, 10.0}};

    book_->yes_book().apply_snapshot(yes_bids, yes_asks);
    book_->no_book().apply_snapshot(no_bids, no_asks);

    EXPECT_DOUBLE_EQ(book_->sum_of_best_asks(), 0.98);  // 0.50 + 0.48
}

TEST_F(BinaryMarketBookTest, SumOfBestBids_CalculatedCorrectly) {
    std::vector<PriceLevel> yes_bids = {{0.48, 10.0}};
    std::vector<PriceLevel> yes_asks = {{0.50, 10.0}};
    std::vector<PriceLevel> no_bids = {{0.45, 10.0}};
    std::vector<PriceLevel> no_asks = {{0.48, 10.0}};

    book_->yes_book().apply_snapshot(yes_bids, yes_asks);
    book_->no_book().apply_snapshot(no_bids, no_asks);

    EXPECT_DOUBLE_EQ(book_->sum_of_best_bids(), 0.93);  // 0.48 + 0.45
}

TEST_F(BinaryMarketBookTest, HasLiquidity_TrueWhenBothSidesHaveLiquidity) {
    std::vector<PriceLevel> bids = {{0.50, 10.0}};
    std::vector<PriceLevel> asks = {{0.52, 10.0}};

    book_->yes_book().apply_snapshot(bids, asks);
    book_->no_book().apply_snapshot(bids, asks);

    EXPECT_TRUE(book_->has_liquidity());
}

TEST_F(BinaryMarketBookTest, HasLiquidity_FalseWhenOneSideEmpty) {
    std::vector<PriceLevel> bids = {{0.50, 10.0}};
    std::vector<PriceLevel> asks = {{0.52, 10.0}};

    book_->yes_book().apply_snapshot(bids, asks);
    // NO book empty

    EXPECT_FALSE(book_->has_liquidity());
}

TEST_F(BinaryMarketBookTest, ImpliedProbability_FromMidPrice) {
    std::vector<PriceLevel> yes_bids = {{0.58, 10.0}};
    std::vector<PriceLevel> yes_asks = {{0.62, 10.0}};
    std::vector<PriceLevel> no_bids = {{0.35, 10.0}};
    std::vector<PriceLevel> no_asks = {{0.40, 10.0}};

    book_->yes_book().apply_snapshot(yes_bids, yes_asks);
    book_->no_book().apply_snapshot(no_bids, no_asks);

    // YES mid = 0.60
    EXPECT_DOUBLE_EQ(book_->yes_implied_probability(), 0.60);
}
