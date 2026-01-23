#include <gtest/gtest.h>
#include "persistence/session_database.hpp"
#include <filesystem>
#include <cstdio>

using namespace arb;

class SessionDatabaseTest : public ::testing::Test {
protected:
    std::string test_db_path_;

    void SetUp() override {
        // Create a unique test database file
        test_db_path_ = "/tmp/test_session_db_" + generate_uuid() + ".db";
    }

    void TearDown() override {
        // Clean up test database
        if (std::filesystem::exists(test_db_path_)) {
            std::filesystem::remove(test_db_path_);
        }
        // Also remove WAL and SHM files if they exist
        std::filesystem::remove(test_db_path_ + "-wal");
        std::filesystem::remove(test_db_path_ + "-shm");
    }
};

// ============================================================================
// Basic Database Tests
// ============================================================================

TEST_F(SessionDatabaseTest, OpensAndInitializesSchema) {
    SessionDatabase db(test_db_path_);
    EXPECT_TRUE(db.is_open());
    db.initialize_schema();
    EXPECT_EQ(db.get_schema_version(), 1);
}

TEST_F(SessionDatabaseTest, UuidGeneration) {
    std::string uuid1 = generate_uuid();
    std::string uuid2 = generate_uuid();

    EXPECT_EQ(uuid1.length(), 36);  // Standard UUID format
    EXPECT_NE(uuid1, uuid2);  // Should be unique
    EXPECT_EQ(uuid1[8], '-');
    EXPECT_EQ(uuid1[13], '-');
    EXPECT_EQ(uuid1[18], '-');
    EXPECT_EQ(uuid1[23], '-');
}

TEST_F(SessionDatabaseTest, TimestampFormatting) {
    int64_t ts = 1705689600000000;  // 2024-01-19 16:00:00 UTC
    std::string formatted = format_timestamp(ts);
    EXPECT_TRUE(formatted.find("2024-01-19") != std::string::npos);
}

// ============================================================================
// Session Tests
// ============================================================================

TEST_F(SessionDatabaseTest, CreateAndRetrieveSession) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.mode = TradingMode::DEMO;
    session.starting_balance = 10000.0;
    session.session_name = "Test Session";
    session.venues_enabled_json = R"(["binance","bybit"])";

    std::string id = db.create_session(session);
    EXPECT_FALSE(id.empty());

    auto retrieved = db.get_session(id);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->session_name, "Test Session");
    EXPECT_EQ(retrieved->mode, TradingMode::DEMO);
    EXPECT_DOUBLE_EQ(retrieved->starting_balance, 10000.0);
}

TEST_F(SessionDatabaseTest, EndSession) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.mode = TradingMode::DEMO;
    session.starting_balance = 10000.0;

    std::string id = db.create_session(session);
    db.end_session(id, 10500.0);

    auto retrieved = db.get_session(id);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_DOUBLE_EQ(retrieved->ending_balance, 10500.0);
    EXPECT_GT(retrieved->end_time, 0);
}

TEST_F(SessionDatabaseTest, ListSessionsOrderedByTime) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session s1, s2, s3;
    s1.session_name = "First";
    s1.starting_balance = 1000;
    s2.session_name = "Second";
    s2.starting_balance = 2000;
    s3.session_name = "Third";
    s3.starting_balance = 3000;

    db.create_session(s1);
    db.create_session(s2);
    db.create_session(s3);

    auto sessions = db.list_sessions(10);
    EXPECT_EQ(sessions.size(), 3);
    // Most recent first
    EXPECT_EQ(sessions[0].session_name, "Third");
}

// ============================================================================
// Order Tests
// ============================================================================

TEST_F(SessionDatabaseTest, InsertAndRetrieveOrders) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    std::string session_id = db.create_session(session);

    Order order;
    order.session_id = session_id;
    order.venue = "binance";
    order.instrument = "BTCUSDT";
    order.side = OrderSide::BUY;
    order.type = OrderType::LIMIT;
    order.price = 45000.0;
    order.qty = 0.1;
    order.status = OrderStatus::PENDING;
    order.reason = OrderReason::ENTRY;

    db.insert_order(order);

    auto orders = db.get_orders_for_session(session_id);
    EXPECT_EQ(orders.size(), 1);
    EXPECT_EQ(orders[0].venue, "binance");
    EXPECT_EQ(orders[0].side, OrderSide::BUY);
    EXPECT_DOUBLE_EQ(orders[0].price, 45000.0);
}

TEST_F(SessionDatabaseTest, UpdateOrderStatus) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    std::string session_id = db.create_session(session);

    Order order;
    order.order_id = generate_uuid();
    order.session_id = session_id;
    order.venue = "binance";
    order.instrument = "BTCUSDT";
    order.side = OrderSide::BUY;
    order.qty = 0.1;
    order.status = OrderStatus::PENDING;
    order.reason = OrderReason::ENTRY;

    db.insert_order(order);
    db.update_order_status(order.order_id, OrderStatus::FILLED);

    auto retrieved = db.get_order(order.order_id);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->status, OrderStatus::FILLED);
}

// ============================================================================
// Fill Tests
// ============================================================================

TEST_F(SessionDatabaseTest, InsertAndRetrieveFills) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    std::string session_id = db.create_session(session);

    Order order;
    order.order_id = generate_uuid();
    order.session_id = session_id;
    order.venue = "binance";
    order.instrument = "BTCUSDT";
    order.side = OrderSide::BUY;
    order.qty = 0.1;
    order.status = OrderStatus::PENDING;
    order.reason = OrderReason::ENTRY;
    db.insert_order(order);

    Fill fill;
    fill.order_id = order.order_id;
    fill.session_id = session_id;
    fill.venue = "binance";
    fill.instrument = "BTCUSDT";
    fill.side = OrderSide::BUY;
    fill.price = 44950.0;
    fill.qty = 0.1;
    fill.fee = 4.495;  // 0.01% fee
    fill.slippage_bps = -5.0;  // Filled better than expected

    db.insert_fill(fill);

    auto fills = db.get_fills_for_session(session_id);
    EXPECT_EQ(fills.size(), 1);
    EXPECT_DOUBLE_EQ(fills[0].price, 44950.0);
    EXPECT_DOUBLE_EQ(fills[0].fee, 4.495);
}

// ============================================================================
// Position Tests
// ============================================================================

TEST_F(SessionDatabaseTest, UpsertPosition) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    std::string session_id = db.create_session(session);

    Position pos1;
    pos1.session_id = session_id;
    pos1.venue = "binance";
    pos1.instrument = "BTCUSDT";
    pos1.qty = 0.1;
    pos1.avg_price = 45000.0;

    db.upsert_position(pos1);

    // Update the same position
    Position pos2;
    pos2.session_id = session_id;
    pos2.venue = "binance";
    pos2.instrument = "BTCUSDT";
    pos2.qty = 0.2;  // Increased
    pos2.avg_price = 44500.0;

    db.upsert_position(pos2);

    auto positions = db.get_positions_for_session(session_id);
    EXPECT_EQ(positions.size(), 1);  // Should still be one position
    EXPECT_DOUBLE_EQ(positions[0].qty, 0.2);
    EXPECT_DOUBLE_EQ(positions[0].avg_price, 44500.0);
}

TEST_F(SessionDatabaseTest, GetSpecificPosition) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    std::string session_id = db.create_session(session);

    Position pos1, pos2;
    pos1.session_id = session_id;
    pos1.venue = "binance";
    pos1.instrument = "BTCUSDT";
    pos1.qty = 0.1;

    pos2.session_id = session_id;
    pos2.venue = "bybit";
    pos2.instrument = "BTCUSDT";
    pos2.qty = -0.1;  // Short position

    db.upsert_position(pos1);
    db.upsert_position(pos2);

    auto binance_pos = db.get_position(session_id, "binance", "BTCUSDT");
    auto bybit_pos = db.get_position(session_id, "bybit", "BTCUSDT");

    ASSERT_TRUE(binance_pos.has_value());
    ASSERT_TRUE(bybit_pos.has_value());
    EXPECT_DOUBLE_EQ(binance_pos->qty, 0.1);
    EXPECT_DOUBLE_EQ(bybit_pos->qty, -0.1);
}

// ============================================================================
// Funding Event Tests
// ============================================================================

TEST_F(SessionDatabaseTest, InsertAndSumFundingEvents) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    std::string session_id = db.create_session(session);

    // Receive funding on short
    FundingEvent e1;
    e1.session_id = session_id;
    e1.venue = "binance";
    e1.instrument = "BTCUSDT";
    e1.funding_rate = 0.0001;
    e1.position_qty = -0.1;
    e1.notional = 4500.0;
    e1.payment_amount = 0.45;  // Positive = received

    // Pay funding on long
    FundingEvent e2;
    e2.session_id = session_id;
    e2.venue = "bybit";
    e2.instrument = "BTCUSDT";
    e2.funding_rate = 0.0002;
    e2.position_qty = 0.1;
    e2.notional = 4500.0;
    e2.payment_amount = -0.90;  // Negative = paid

    db.insert_funding_event(e1);
    db.insert_funding_event(e2);

    auto events = db.get_funding_events_for_session(session_id);
    EXPECT_EQ(events.size(), 2);

    double total = db.get_total_funding_for_session(session_id);
    EXPECT_NEAR(total, -0.45, 0.001);  // Net funding paid
}

// ============================================================================
// PnL Snapshot Tests
// ============================================================================

TEST_F(SessionDatabaseTest, InsertAndRetrievePnlSnapshots) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    std::string session_id = db.create_session(session);

    PnlSnapshot snap1, snap2;
    snap1.session_id = session_id;
    snap1.cash_balance = 10000;
    snap1.equity = 10000;
    snap1.unrealized_pnl = 0;
    snap1.realized_pnl = 0;
    snap1.pnl_funding = 0;
    snap1.pnl_fees = 0;
    snap1.pnl_basis = 0;

    snap2.session_id = session_id;
    snap2.cash_balance = 9950;  // Some cash used for margin
    snap2.equity = 10050;       // Unrealized gain
    snap2.unrealized_pnl = 100;
    snap2.realized_pnl = 0;
    snap2.pnl_funding = 5;
    snap2.pnl_fees = -5;
    snap2.pnl_basis = 0;
    snap2.leverage = 2.0;
    snap2.drawdown = 0;
    snap2.high_water_mark = 10050;

    db.insert_pnl_snapshot(snap1);
    db.insert_pnl_snapshot(snap2);

    auto latest = db.get_latest_pnl_snapshot(session_id);
    ASSERT_TRUE(latest.has_value());
    EXPECT_DOUBLE_EQ(latest->equity, 10050);
    EXPECT_DOUBLE_EQ(latest->pnl_funding, 5);
}

// ============================================================================
// Kill Event Tests
// ============================================================================

TEST_F(SessionDatabaseTest, InsertAndRetrieveKillEvents) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    std::string session_id = db.create_session(session);

    KillEvent kill;
    kill.session_id = session_id;
    kill.reason_code = "BASIS_DIVERGENCE";
    kill.reason_detail = "Basis diverged 1.5% > 0.5% limit";
    kill.positions_closed_json = R"([{"venue":"binance","qty":0.1},{"venue":"bybit","qty":-0.1}])";
    kill.pnl_impact = -50.0;

    db.insert_kill_event(kill);

    auto events = db.get_kill_events_for_session(session_id);
    EXPECT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].reason_code, "BASIS_DIVERGENCE");
    EXPECT_DOUBLE_EQ(events[0].pnl_impact, -50.0);
}

// ============================================================================
// Reporting Tests
// ============================================================================

TEST_F(SessionDatabaseTest, ComputeSessionSummary) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    session.session_name = "Test Summary";
    std::string session_id = db.create_session(session);

    // Add some fills with fees
    Order order;
    order.order_id = generate_uuid();
    order.session_id = session_id;
    order.venue = "binance";
    order.instrument = "BTCUSDT";
    order.side = OrderSide::BUY;
    order.qty = 0.1;
    order.reason = OrderReason::ENTRY;
    db.insert_order(order);

    Fill fill;
    fill.order_id = order.order_id;
    fill.session_id = session_id;
    fill.venue = "binance";
    fill.instrument = "BTCUSDT";
    fill.side = OrderSide::BUY;
    fill.price = 45000;
    fill.qty = 0.1;
    fill.fee = 4.5;
    db.insert_fill(fill);

    // Add funding event
    FundingEvent funding;
    funding.session_id = session_id;
    funding.venue = "binance";
    funding.instrument = "BTCUSDT";
    funding.payment_amount = 10.0;
    db.insert_funding_event(funding);

    // End session with profit
    db.end_session(session_id, 10100);

    auto summary = db.compute_session_summary(session_id);
    EXPECT_EQ(summary.session_name, "Test Summary");
    EXPECT_DOUBLE_EQ(summary.starting_balance, 10000);
    EXPECT_DOUBLE_EQ(summary.ending_balance, 10100);
    EXPECT_DOUBLE_EQ(summary.total_pnl, 100);
    EXPECT_DOUBLE_EQ(summary.pnl_funding, 10);
    EXPECT_DOUBLE_EQ(summary.pnl_fees, -4.5);
    EXPECT_EQ(summary.total_fills, 1);
    EXPECT_EQ(summary.total_funding_events, 1);
}

TEST_F(SessionDatabaseTest, GenerateReport) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    session.session_name = "Report Test";
    std::string session_id = db.create_session(session);
    db.end_session(session_id, 10500);

    std::string report = db.generate_report(session_id);

    // Check that report contains expected sections
    EXPECT_TRUE(report.find("SESSION REPORT") != std::string::npos);
    EXPECT_TRUE(report.find("PnL BREAKDOWN") != std::string::npos);
    EXPECT_TRUE(report.find("10000") != std::string::npos);  // Starting balance
    EXPECT_TRUE(report.find("10500") != std::string::npos);  // Ending balance
}

// ============================================================================
// String Conversion Tests
// ============================================================================

TEST_F(SessionDatabaseTest, EnumStringConversions) {
    EXPECT_EQ(to_string(TradingMode::DEMO), "demo");
    EXPECT_EQ(to_string(TradingMode::LIVE), "live");

    EXPECT_EQ(to_string(OrderSide::BUY), "buy");
    EXPECT_EQ(to_string(OrderSide::SELL), "sell");

    EXPECT_EQ(to_string(OrderStatus::FILLED), "filled");
    EXPECT_EQ(to_string(OrderStatus::REJECTED), "rejected");

    EXPECT_EQ(to_string(OrderReason::HEDGE), "hedge");
    EXPECT_EQ(to_string(OrderReason::KILL), "kill");

    EXPECT_EQ(trading_mode_from_string("live"), TradingMode::LIVE);
    EXPECT_EQ(order_side_from_string("sell"), OrderSide::SELL);
    EXPECT_EQ(order_status_from_string("cancelled"), OrderStatus::CANCELLED);
    EXPECT_EQ(order_reason_from_string("rebalance"), OrderReason::REBALANCE);
}

// ============================================================================
// Transaction Tests
// ============================================================================

TEST_F(SessionDatabaseTest, TransactionCommit) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    db.begin_transaction();

    Session session;
    session.starting_balance = 10000;
    std::string session_id = db.create_session(session);

    db.commit_transaction();

    auto retrieved = db.get_session(session_id);
    ASSERT_TRUE(retrieved.has_value());
}

TEST_F(SessionDatabaseTest, TransactionRollback) {
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    // Create a session outside transaction
    Session session1;
    session1.starting_balance = 5000;
    std::string id1 = db.create_session(session1);

    // Start transaction, create another session, then rollback
    db.begin_transaction();

    Session session2;
    session2.starting_balance = 10000;
    std::string id2 = db.create_session(session2);

    db.rollback_transaction();

    // First session should exist
    auto retrieved1 = db.get_session(id1);
    ASSERT_TRUE(retrieved1.has_value());

    // Second session should not exist after rollback
    auto retrieved2 = db.get_session(id2);
    EXPECT_FALSE(retrieved2.has_value());
}

// ============================================================================
// PnL Reconciliation Test
// ============================================================================

TEST_F(SessionDatabaseTest, PnlReconciliation) {
    // This test verifies that equity = cash + positions + funding - fees
    SessionDatabase db(test_db_path_);
    db.initialize_schema();

    Session session;
    session.starting_balance = 10000;
    std::string session_id = db.create_session(session);

    // Simulate a complete trading sequence:
    // 1. Enter long on binance (pay taker fee)
    // 2. Enter short on bybit (pay taker fee)
    // 3. Receive funding payment (short pays long)
    // 4. Exit both positions

    double cash = 10000;
    double fees_paid = 0;
    double funding_received = 0;

    // Create orders first (required for foreign key constraint)
    Order order_entry_long;
    order_entry_long.order_id = generate_uuid();
    order_entry_long.session_id = session_id;
    order_entry_long.venue = "binance";
    order_entry_long.instrument = "BTCUSDT";
    order_entry_long.side = OrderSide::BUY;
    order_entry_long.qty = 0.1;
    order_entry_long.reason = OrderReason::ENTRY;
    db.insert_order(order_entry_long);

    Order order_entry_short;
    order_entry_short.order_id = generate_uuid();
    order_entry_short.session_id = session_id;
    order_entry_short.venue = "bybit";
    order_entry_short.instrument = "BTCUSDT";
    order_entry_short.side = OrderSide::SELL;
    order_entry_short.qty = 0.1;
    order_entry_short.reason = OrderReason::ENTRY;
    db.insert_order(order_entry_short);

    Order order_exit_long;
    order_exit_long.order_id = generate_uuid();
    order_exit_long.session_id = session_id;
    order_exit_long.venue = "binance";
    order_exit_long.instrument = "BTCUSDT";
    order_exit_long.side = OrderSide::SELL;
    order_exit_long.qty = 0.1;
    order_exit_long.reason = OrderReason::EXIT;
    db.insert_order(order_exit_long);

    Order order_exit_short;
    order_exit_short.order_id = generate_uuid();
    order_exit_short.session_id = session_id;
    order_exit_short.venue = "bybit";
    order_exit_short.instrument = "BTCUSDT";
    order_exit_short.side = OrderSide::BUY;
    order_exit_short.qty = 0.1;
    order_exit_short.reason = OrderReason::EXIT;
    db.insert_order(order_exit_short);

    // Entry fills (linked to orders)
    Fill entry_long;
    entry_long.session_id = session_id;
    entry_long.order_id = order_entry_long.order_id;
    entry_long.venue = "binance";
    entry_long.instrument = "BTCUSDT";
    entry_long.side = OrderSide::BUY;
    entry_long.price = 45000;
    entry_long.qty = 0.1;
    entry_long.fee = 2.25;  // 0.05% taker
    fees_paid += entry_long.fee;

    Fill entry_short;
    entry_short.session_id = session_id;
    entry_short.order_id = order_entry_short.order_id;
    entry_short.venue = "bybit";
    entry_short.instrument = "BTCUSDT";
    entry_short.side = OrderSide::SELL;
    entry_short.price = 45010;
    entry_short.qty = 0.1;
    entry_short.fee = 2.25;
    fees_paid += entry_short.fee;

    db.insert_fill(entry_long);
    db.insert_fill(entry_short);

    // Funding event: short receives funding
    FundingEvent funding;
    funding.session_id = session_id;
    funding.venue = "bybit";
    funding.instrument = "BTCUSDT";
    funding.funding_rate = 0.0001;
    funding.position_qty = -0.1;
    funding.notional = 4501;
    funding.payment_amount = 0.45;  // Received
    funding_received += funding.payment_amount;
    db.insert_funding_event(funding);

    // Exit fills (price moved up slightly)
    Fill exit_long;
    exit_long.session_id = session_id;
    exit_long.order_id = order_exit_long.order_id;
    exit_long.venue = "binance";
    exit_long.instrument = "BTCUSDT";
    exit_long.side = OrderSide::SELL;
    exit_long.price = 45100;
    exit_long.qty = 0.1;
    exit_long.fee = 2.255;
    fees_paid += exit_long.fee;
    double long_pnl = (45100 - 45000) * 0.1;  // +$10

    Fill exit_short;
    exit_short.session_id = session_id;
    exit_short.order_id = order_exit_short.order_id;
    exit_short.venue = "bybit";
    exit_short.instrument = "BTCUSDT";
    exit_short.side = OrderSide::BUY;
    exit_short.price = 45090;
    exit_short.qty = 0.1;
    exit_short.fee = 2.254;
    fees_paid += exit_short.fee;
    double short_pnl = (45010 - 45090) * 0.1;  // -$8

    db.insert_fill(exit_long);
    db.insert_fill(exit_short);

    // Calculate expected PnL
    double expected_pnl = long_pnl + short_pnl + funding_received - fees_paid;
    double ending_balance = cash + expected_pnl;

    db.end_session(session_id, ending_balance);

    // Verify reconciliation
    auto summary = db.compute_session_summary(session_id);
    EXPECT_NEAR(summary.total_pnl, expected_pnl, 0.01);
    EXPECT_NEAR(summary.pnl_funding, funding_received, 0.001);
    EXPECT_NEAR(summary.pnl_fees, -fees_paid, 0.01);

    // The key invariant: total_pnl should equal the sum of components
    double reconciled_pnl = (long_pnl + short_pnl) + summary.pnl_funding + summary.pnl_fees;
    EXPECT_NEAR(summary.total_pnl, reconciled_pnl, 0.01);
}
