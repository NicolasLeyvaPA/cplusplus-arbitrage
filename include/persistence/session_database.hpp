#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <chrono>
#include <map>
#include <cstdint>

// Forward declare sqlite3
struct sqlite3;
struct sqlite3_stmt;

namespace arb {

// ============================================================================
// AUDIT-GRADE SESSION DATABASE
//
// This is the single source of truth for all trading activity.
// Every fill, every funding payment, every PnL snapshot.
//
// Design principles:
// 1. All PnL must reconcile: equity = cash + positions + funding - fees
// 2. Every session is immutable once ended
// 3. All timestamps are UTC microseconds
// 4. UUIDs for all primary keys (reproducible via session_id + sequence)
// ============================================================================

// UUID generation (simple but sufficient)
std::string generate_uuid();

// Timestamp utilities
int64_t now_micros();
std::string format_timestamp(int64_t micros);

// ============================================================================
// DATA STRUCTURES
// ============================================================================

enum class TradingMode {
    DEMO,
    LIVE
};

enum class OrderSide {
    BUY,
    SELL
};

enum class OrderType {
    MARKET,
    LIMIT,
    IOC
};

enum class OrderStatus {
    PENDING,
    FILLED,
    PARTIALLY_FILLED,
    CANCELLED,
    REJECTED
};

enum class OrderReason {
    ENTRY,          // Opening a position
    HEDGE,          // Neutralizing delta
    EXIT,           // Closing position (normal)
    KILL,           // Emergency close (risk limit)
    REBALANCE       // Adjusting allocation
};

struct Session {
    std::string session_id;
    int64_t start_time{0};
    int64_t end_time{0};
    TradingMode mode{TradingMode::DEMO};
    double starting_balance{0};
    double ending_balance{0};
    std::string venues_enabled_json;
    std::string config_json;
    std::string git_commit_hash;
    std::string notes;
    std::string session_name;
};

struct Order {
    std::string order_id;
    std::string session_id;
    std::string venue;
    std::string instrument;
    OrderSide side{OrderSide::BUY};
    OrderType type{OrderType::MARKET};
    double price{0};
    double qty{0};
    OrderStatus status{OrderStatus::PENDING};
    int64_t created_at{0};
    int64_t updated_at{0};
    OrderReason reason{OrderReason::ENTRY};
    std::string reason_detail;
};

struct Fill {
    std::string fill_id;
    std::string order_id;
    std::string session_id;
    std::string venue;
    std::string instrument;
    OrderSide side{OrderSide::BUY};
    double price{0};
    double qty{0};
    double fee{0};
    int64_t timestamp{0};
    double slippage_bps{0};
    int64_t latency_ms{0};
};

struct Position {
    std::string position_id;
    std::string session_id;
    std::string venue;
    std::string instrument;
    double qty{0};              // Positive = long, negative = short
    double avg_price{0};
    double mark_price{0};
    double unrealized_pnl{0};
    double margin_used{0};
    int64_t updated_at{0};
};

struct FundingEvent {
    std::string funding_event_id;
    std::string session_id;
    std::string venue;
    std::string instrument;
    double funding_rate{0};
    double position_qty{0};
    double notional{0};
    double payment_amount{0};   // Positive = received, negative = paid
    int64_t timestamp{0};
    int64_t next_funding_time{0};
};

struct PnlSnapshot {
    std::string snapshot_id;
    std::string session_id;
    int64_t timestamp{0};
    double cash_balance{0};
    double equity{0};
    double unrealized_pnl{0};
    double realized_pnl{0};
    double pnl_funding{0};
    double pnl_fees{0};
    double pnl_basis{0};
    std::string exposure_json;
    double leverage{0};
    double drawdown{0};
    double high_water_mark{0};
};

struct KillEvent {
    std::string kill_event_id;
    std::string session_id;
    std::string reason_code;
    std::string reason_detail;
    int64_t timestamp{0};
    std::string positions_closed_json;
    double pnl_impact{0};
};

// ============================================================================
// SESSION SUMMARY (for reporting)
// ============================================================================

struct SessionSummary {
    std::string session_id;
    std::string session_name;
    int64_t start_time{0};
    int64_t end_time{0};
    TradingMode mode{TradingMode::DEMO};
    double starting_balance{0};
    double ending_balance{0};

    // Computed metrics
    double total_pnl{0};
    double pnl_funding{0};
    double pnl_fees{0};
    double pnl_basis{0};
    double pnl_realized{0};
    double max_drawdown{0};
    double sharpe_estimate{0};

    int total_fills{0};
    int total_funding_events{0};
    int total_kill_events{0};
    int hedge_count{0};

    double pct_time_neutral{0};
    std::map<std::string, double> pnl_by_venue;
};

// ============================================================================
// DATABASE CLASS
// ============================================================================

class SessionDatabase {
public:
    explicit SessionDatabase(const std::string& db_path);
    ~SessionDatabase();

    // Non-copyable
    SessionDatabase(const SessionDatabase&) = delete;
    SessionDatabase& operator=(const SessionDatabase&) = delete;

    // Connection management
    bool is_open() const;
    void close();

    // Schema management
    void initialize_schema();
    int get_schema_version();
    void migrate_schema();

    // Session management
    std::string create_session(const Session& session);
    void end_session(const std::string& session_id, double ending_balance);
    std::optional<Session> get_session(const std::string& session_id);
    std::vector<Session> list_sessions(int limit = 100);
    std::optional<Session> get_latest_session();

    // Orders
    void insert_order(const Order& order);
    void update_order_status(const std::string& order_id, OrderStatus status);
    std::vector<Order> get_orders_for_session(const std::string& session_id);
    std::optional<Order> get_order(const std::string& order_id);

    // Fills
    void insert_fill(const Fill& fill);
    std::vector<Fill> get_fills_for_session(const std::string& session_id);
    std::vector<Fill> get_fills_for_order(const std::string& order_id);

    // Positions
    void upsert_position(const Position& position);
    std::vector<Position> get_positions_for_session(const std::string& session_id);
    std::optional<Position> get_position(const std::string& session_id,
                                         const std::string& venue,
                                         const std::string& instrument);
    void clear_positions_for_session(const std::string& session_id);

    // Funding events
    void insert_funding_event(const FundingEvent& event);
    std::vector<FundingEvent> get_funding_events_for_session(const std::string& session_id);
    double get_total_funding_for_session(const std::string& session_id);

    // PnL snapshots
    void insert_pnl_snapshot(const PnlSnapshot& snapshot);
    std::vector<PnlSnapshot> get_pnl_snapshots_for_session(const std::string& session_id);
    std::optional<PnlSnapshot> get_latest_pnl_snapshot(const std::string& session_id);

    // Kill events
    void insert_kill_event(const KillEvent& event);
    std::vector<KillEvent> get_kill_events_for_session(const std::string& session_id);

    // Reporting
    SessionSummary compute_session_summary(const std::string& session_id);
    std::string generate_report(const std::string& session_id);

    // CSV export
    void export_fills_csv(const std::string& session_id, const std::string& path);
    void export_funding_events_csv(const std::string& session_id, const std::string& path);
    void export_pnl_snapshots_csv(const std::string& session_id, const std::string& path);

    // Utility
    void execute(const std::string& sql);
    void begin_transaction();
    void commit_transaction();
    void rollback_transaction();

private:
    sqlite3* db_{nullptr};
    std::string db_path_;
    bool in_transaction_{false};

    // Statement preparation helpers
    sqlite3_stmt* prepare(const std::string& sql);
    void bind_text(sqlite3_stmt* stmt, int index, const std::string& value);
    void bind_int64(sqlite3_stmt* stmt, int index, int64_t value);
    void bind_double(sqlite3_stmt* stmt, int index, double value);
    void finalize(sqlite3_stmt* stmt);

    // Result extraction helpers
    std::string get_text(sqlite3_stmt* stmt, int col);
    int64_t get_int64(sqlite3_stmt* stmt, int col);
    double get_double(sqlite3_stmt* stmt, int col);

    // Schema creation
    void create_tables();
    void create_indexes();
};

// ============================================================================
// STRING CONVERSIONS
// ============================================================================

std::string to_string(TradingMode mode);
std::string to_string(OrderSide side);
std::string to_string(OrderType type);
std::string to_string(OrderStatus status);
std::string to_string(OrderReason reason);

TradingMode trading_mode_from_string(const std::string& s);
OrderSide order_side_from_string(const std::string& s);
OrderType order_type_from_string(const std::string& s);
OrderStatus order_status_from_string(const std::string& s);
OrderReason order_reason_from_string(const std::string& s);

}  // namespace arb
