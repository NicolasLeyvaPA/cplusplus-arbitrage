#include "persistence/session_database.hpp"
#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <stdexcept>

namespace arb {

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    uint64_t a = dis(gen);
    uint64_t b = dis(gen);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(8) << ((a >> 32) & 0xFFFFFFFF);
    ss << "-";
    ss << std::setw(4) << ((a >> 16) & 0xFFFF);
    ss << "-";
    ss << std::setw(4) << (((a & 0xFFFF) & 0x0FFF) | 0x4000);  // Version 4
    ss << "-";
    ss << std::setw(4) << (((b >> 48) & 0x3FFF) | 0x8000);  // Variant
    ss << "-";
    ss << std::setw(12) << (b & 0xFFFFFFFFFFFF);

    return ss.str();
}

int64_t now_micros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string format_timestamp(int64_t micros) {
    auto seconds = micros / 1000000;
    auto us = micros % 1000000;
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm tm = *std::gmtime(&t);

    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(6) << us;
    return ss.str();
}

// ============================================================================
// STRING CONVERSIONS
// ============================================================================

std::string to_string(TradingMode mode) {
    switch (mode) {
        case TradingMode::DEMO: return "demo";
        case TradingMode::LIVE: return "live";
    }
    return "unknown";
}

std::string to_string(OrderSide side) {
    switch (side) {
        case OrderSide::BUY: return "buy";
        case OrderSide::SELL: return "sell";
    }
    return "unknown";
}

std::string to_string(OrderType type) {
    switch (type) {
        case OrderType::MARKET: return "market";
        case OrderType::LIMIT: return "limit";
        case OrderType::IOC: return "ioc";
    }
    return "unknown";
}

std::string to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::PENDING: return "pending";
        case OrderStatus::FILLED: return "filled";
        case OrderStatus::PARTIALLY_FILLED: return "partially_filled";
        case OrderStatus::CANCELLED: return "cancelled";
        case OrderStatus::REJECTED: return "rejected";
    }
    return "unknown";
}

std::string to_string(OrderReason reason) {
    switch (reason) {
        case OrderReason::ENTRY: return "entry";
        case OrderReason::HEDGE: return "hedge";
        case OrderReason::EXIT: return "exit";
        case OrderReason::KILL: return "kill";
        case OrderReason::REBALANCE: return "rebalance";
    }
    return "unknown";
}

TradingMode trading_mode_from_string(const std::string& s) {
    if (s == "live") return TradingMode::LIVE;
    return TradingMode::DEMO;
}

OrderSide order_side_from_string(const std::string& s) {
    if (s == "sell") return OrderSide::SELL;
    return OrderSide::BUY;
}

OrderType order_type_from_string(const std::string& s) {
    if (s == "limit") return OrderType::LIMIT;
    if (s == "ioc") return OrderType::IOC;
    return OrderType::MARKET;
}

OrderStatus order_status_from_string(const std::string& s) {
    if (s == "filled") return OrderStatus::FILLED;
    if (s == "partially_filled") return OrderStatus::PARTIALLY_FILLED;
    if (s == "cancelled") return OrderStatus::CANCELLED;
    if (s == "rejected") return OrderStatus::REJECTED;
    return OrderStatus::PENDING;
}

OrderReason order_reason_from_string(const std::string& s) {
    if (s == "hedge") return OrderReason::HEDGE;
    if (s == "exit") return OrderReason::EXIT;
    if (s == "kill") return OrderReason::KILL;
    if (s == "rebalance") return OrderReason::REBALANCE;
    return OrderReason::ENTRY;
}

// ============================================================================
// DATABASE IMPLEMENTATION
// ============================================================================

SessionDatabase::SessionDatabase(const std::string& db_path)
    : db_path_(db_path)
{
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Failed to open database: " + error);
    }

    // Enable foreign keys
    execute("PRAGMA foreign_keys = ON;");

    // WAL mode for better concurrent access
    execute("PRAGMA journal_mode = WAL;");

    spdlog::info("SessionDatabase opened: {}", db_path);
}

SessionDatabase::~SessionDatabase() {
    close();
}

bool SessionDatabase::is_open() const {
    return db_ != nullptr;
}

void SessionDatabase::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
        spdlog::info("SessionDatabase closed");
    }
}

void SessionDatabase::execute(const std::string& sql) {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string error = errmsg ? errmsg : "Unknown error";
        sqlite3_free(errmsg);
        throw std::runtime_error("SQL error: " + error + " in: " + sql);
    }
}

void SessionDatabase::begin_transaction() {
    if (!in_transaction_) {
        execute("BEGIN TRANSACTION;");
        in_transaction_ = true;
    }
}

void SessionDatabase::commit_transaction() {
    if (in_transaction_) {
        execute("COMMIT;");
        in_transaction_ = false;
    }
}

void SessionDatabase::rollback_transaction() {
    if (in_transaction_) {
        execute("ROLLBACK;");
        in_transaction_ = false;
    }
}

sqlite3_stmt* SessionDatabase::prepare(const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare statement: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    return stmt;
}

void SessionDatabase::bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

void SessionDatabase::bind_int64(sqlite3_stmt* stmt, int index, int64_t value) {
    sqlite3_bind_int64(stmt, index, value);
}

void SessionDatabase::bind_double(sqlite3_stmt* stmt, int index, double value) {
    sqlite3_bind_double(stmt, index, value);
}

void SessionDatabase::finalize(sqlite3_stmt* stmt) {
    sqlite3_finalize(stmt);
}

std::string SessionDatabase::get_text(sqlite3_stmt* stmt, int col) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text ? text : "";
}

int64_t SessionDatabase::get_int64(sqlite3_stmt* stmt, int col) {
    return sqlite3_column_int64(stmt, col);
}

double SessionDatabase::get_double(sqlite3_stmt* stmt, int col) {
    return sqlite3_column_double(stmt, col);
}

// ============================================================================
// SCHEMA
// ============================================================================

void SessionDatabase::initialize_schema() {
    create_tables();
    create_indexes();
    spdlog::info("Database schema initialized");
}

void SessionDatabase::create_tables() {
    // Sessions table
    execute(R"(
        CREATE TABLE IF NOT EXISTS sessions (
            session_id TEXT PRIMARY KEY,
            start_time INTEGER NOT NULL,
            end_time INTEGER,
            mode TEXT NOT NULL,
            starting_balance REAL NOT NULL,
            ending_balance REAL,
            venues_enabled_json TEXT,
            config_json TEXT,
            git_commit_hash TEXT,
            notes TEXT,
            session_name TEXT
        );
    )");

    // Orders table
    execute(R"(
        CREATE TABLE IF NOT EXISTS orders (
            order_id TEXT PRIMARY KEY,
            session_id TEXT NOT NULL,
            venue TEXT NOT NULL,
            instrument TEXT NOT NULL,
            side TEXT NOT NULL,
            type TEXT NOT NULL,
            price REAL,
            qty REAL NOT NULL,
            status TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            reason TEXT NOT NULL,
            reason_detail TEXT,
            FOREIGN KEY (session_id) REFERENCES sessions(session_id)
        );
    )");

    // Fills table
    execute(R"(
        CREATE TABLE IF NOT EXISTS fills (
            fill_id TEXT PRIMARY KEY,
            order_id TEXT NOT NULL,
            session_id TEXT NOT NULL,
            venue TEXT NOT NULL,
            instrument TEXT NOT NULL,
            side TEXT NOT NULL,
            price REAL NOT NULL,
            qty REAL NOT NULL,
            fee REAL NOT NULL,
            timestamp INTEGER NOT NULL,
            slippage_bps REAL,
            latency_ms INTEGER,
            FOREIGN KEY (order_id) REFERENCES orders(order_id),
            FOREIGN KEY (session_id) REFERENCES sessions(session_id)
        );
    )");

    // Positions table
    execute(R"(
        CREATE TABLE IF NOT EXISTS positions (
            position_id TEXT PRIMARY KEY,
            session_id TEXT NOT NULL,
            venue TEXT NOT NULL,
            instrument TEXT NOT NULL,
            qty REAL NOT NULL,
            avg_price REAL NOT NULL,
            mark_price REAL,
            unrealized_pnl REAL,
            margin_used REAL,
            updated_at INTEGER NOT NULL,
            FOREIGN KEY (session_id) REFERENCES sessions(session_id),
            UNIQUE(session_id, venue, instrument)
        );
    )");

    // Funding events table
    execute(R"(
        CREATE TABLE IF NOT EXISTS funding_events (
            funding_event_id TEXT PRIMARY KEY,
            session_id TEXT NOT NULL,
            venue TEXT NOT NULL,
            instrument TEXT NOT NULL,
            funding_rate REAL NOT NULL,
            position_qty REAL NOT NULL,
            notional REAL NOT NULL,
            payment_amount REAL NOT NULL,
            timestamp INTEGER NOT NULL,
            next_funding_time INTEGER,
            FOREIGN KEY (session_id) REFERENCES sessions(session_id)
        );
    )");

    // PnL snapshots table
    execute(R"(
        CREATE TABLE IF NOT EXISTS pnl_snapshots (
            snapshot_id TEXT PRIMARY KEY,
            session_id TEXT NOT NULL,
            timestamp INTEGER NOT NULL,
            cash_balance REAL NOT NULL,
            equity REAL NOT NULL,
            unrealized_pnl REAL NOT NULL,
            realized_pnl REAL NOT NULL,
            pnl_funding REAL NOT NULL,
            pnl_fees REAL NOT NULL,
            pnl_basis REAL NOT NULL,
            exposure_json TEXT,
            leverage REAL,
            drawdown REAL,
            high_water_mark REAL,
            FOREIGN KEY (session_id) REFERENCES sessions(session_id)
        );
    )");

    // Kill events table
    execute(R"(
        CREATE TABLE IF NOT EXISTS kill_events (
            kill_event_id TEXT PRIMARY KEY,
            session_id TEXT NOT NULL,
            reason_code TEXT NOT NULL,
            reason_detail TEXT,
            timestamp INTEGER NOT NULL,
            positions_closed_json TEXT,
            pnl_impact REAL,
            FOREIGN KEY (session_id) REFERENCES sessions(session_id)
        );
    )");

    // Schema version table
    execute(R"(
        CREATE TABLE IF NOT EXISTS schema_version (
            version INTEGER PRIMARY KEY
        );
    )");

    // Insert initial version if not exists
    execute("INSERT OR IGNORE INTO schema_version (version) VALUES (1);");
}

void SessionDatabase::create_indexes() {
    execute("CREATE INDEX IF NOT EXISTS idx_orders_session ON orders(session_id);");
    execute("CREATE INDEX IF NOT EXISTS idx_fills_session ON fills(session_id);");
    execute("CREATE INDEX IF NOT EXISTS idx_fills_order ON fills(order_id);");
    execute("CREATE INDEX IF NOT EXISTS idx_positions_session ON positions(session_id);");
    execute("CREATE INDEX IF NOT EXISTS idx_funding_session ON funding_events(session_id);");
    execute("CREATE INDEX IF NOT EXISTS idx_pnl_session ON pnl_snapshots(session_id);");
    execute("CREATE INDEX IF NOT EXISTS idx_kill_session ON kill_events(session_id);");
    execute("CREATE INDEX IF NOT EXISTS idx_sessions_time ON sessions(start_time DESC);");
}

int SessionDatabase::get_schema_version() {
    auto stmt = prepare("SELECT version FROM schema_version LIMIT 1;");
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = static_cast<int>(get_int64(stmt, 0));
    }
    finalize(stmt);
    return version;
}

void SessionDatabase::migrate_schema() {
    // Future migrations go here
    int current = get_schema_version();
    spdlog::info("Current schema version: {}", current);
}

// ============================================================================
// SESSION OPERATIONS
// ============================================================================

std::string SessionDatabase::create_session(const Session& session) {
    std::string id = session.session_id.empty() ? generate_uuid() : session.session_id;

    auto stmt = prepare(R"(
        INSERT INTO sessions (
            session_id, start_time, mode, starting_balance,
            venues_enabled_json, config_json, git_commit_hash, notes, session_name
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    )");

    bind_text(stmt, 1, id);
    bind_int64(stmt, 2, session.start_time ? session.start_time : now_micros());
    bind_text(stmt, 3, to_string(session.mode));
    bind_double(stmt, 4, session.starting_balance);
    bind_text(stmt, 5, session.venues_enabled_json);
    bind_text(stmt, 6, session.config_json);
    bind_text(stmt, 7, session.git_commit_hash);
    bind_text(stmt, 8, session.notes);
    bind_text(stmt, 9, session.session_name);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        finalize(stmt);
        throw std::runtime_error("Failed to create session");
    }

    finalize(stmt);
    spdlog::info("Created session: {} ({})", id, session.session_name);
    return id;
}

void SessionDatabase::end_session(const std::string& session_id, double ending_balance) {
    auto stmt = prepare(R"(
        UPDATE sessions SET end_time = ?, ending_balance = ? WHERE session_id = ?;
    )");

    bind_int64(stmt, 1, now_micros());
    bind_double(stmt, 2, ending_balance);
    bind_text(stmt, 3, session_id);

    sqlite3_step(stmt);
    finalize(stmt);
    spdlog::info("Ended session: {}, final balance: ${:.2f}", session_id, ending_balance);
}

std::optional<Session> SessionDatabase::get_session(const std::string& session_id) {
    auto stmt = prepare("SELECT * FROM sessions WHERE session_id = ?;");
    bind_text(stmt, 1, session_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        finalize(stmt);
        return std::nullopt;
    }

    Session s;
    s.session_id = get_text(stmt, 0);
    s.start_time = get_int64(stmt, 1);
    s.end_time = get_int64(stmt, 2);
    s.mode = trading_mode_from_string(get_text(stmt, 3));
    s.starting_balance = get_double(stmt, 4);
    s.ending_balance = get_double(stmt, 5);
    s.venues_enabled_json = get_text(stmt, 6);
    s.config_json = get_text(stmt, 7);
    s.git_commit_hash = get_text(stmt, 8);
    s.notes = get_text(stmt, 9);
    s.session_name = get_text(stmt, 10);

    finalize(stmt);
    return s;
}

std::vector<Session> SessionDatabase::list_sessions(int limit) {
    auto stmt = prepare(
        "SELECT * FROM sessions ORDER BY start_time DESC LIMIT ?;"
    );
    bind_int64(stmt, 1, limit);

    std::vector<Session> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Session s;
        s.session_id = get_text(stmt, 0);
        s.start_time = get_int64(stmt, 1);
        s.end_time = get_int64(stmt, 2);
        s.mode = trading_mode_from_string(get_text(stmt, 3));
        s.starting_balance = get_double(stmt, 4);
        s.ending_balance = get_double(stmt, 5);
        s.venues_enabled_json = get_text(stmt, 6);
        s.config_json = get_text(stmt, 7);
        s.git_commit_hash = get_text(stmt, 8);
        s.notes = get_text(stmt, 9);
        s.session_name = get_text(stmt, 10);
        result.push_back(s);
    }

    finalize(stmt);
    return result;
}

std::optional<Session> SessionDatabase::get_latest_session() {
    auto sessions = list_sessions(1);
    if (sessions.empty()) return std::nullopt;
    return sessions[0];
}

// ============================================================================
// ORDER OPERATIONS
// ============================================================================

void SessionDatabase::insert_order(const Order& order) {
    auto stmt = prepare(R"(
        INSERT INTO orders (
            order_id, session_id, venue, instrument, side, type,
            price, qty, status, created_at, updated_at, reason, reason_detail
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )");

    std::string id = order.order_id.empty() ? generate_uuid() : order.order_id;

    bind_text(stmt, 1, id);
    bind_text(stmt, 2, order.session_id);
    bind_text(stmt, 3, order.venue);
    bind_text(stmt, 4, order.instrument);
    bind_text(stmt, 5, to_string(order.side));
    bind_text(stmt, 6, to_string(order.type));
    bind_double(stmt, 7, order.price);
    bind_double(stmt, 8, order.qty);
    bind_text(stmt, 9, to_string(order.status));
    bind_int64(stmt, 10, order.created_at ? order.created_at : now_micros());
    bind_int64(stmt, 11, order.updated_at ? order.updated_at : now_micros());
    bind_text(stmt, 12, to_string(order.reason));
    bind_text(stmt, 13, order.reason_detail);

    sqlite3_step(stmt);
    finalize(stmt);
}

void SessionDatabase::update_order_status(const std::string& order_id, OrderStatus status) {
    auto stmt = prepare(R"(
        UPDATE orders SET status = ?, updated_at = ? WHERE order_id = ?;
    )");

    bind_text(stmt, 1, to_string(status));
    bind_int64(stmt, 2, now_micros());
    bind_text(stmt, 3, order_id);

    sqlite3_step(stmt);
    finalize(stmt);
}

std::vector<Order> SessionDatabase::get_orders_for_session(const std::string& session_id) {
    auto stmt = prepare(
        "SELECT * FROM orders WHERE session_id = ? ORDER BY created_at;"
    );
    bind_text(stmt, 1, session_id);

    std::vector<Order> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Order o;
        o.order_id = get_text(stmt, 0);
        o.session_id = get_text(stmt, 1);
        o.venue = get_text(stmt, 2);
        o.instrument = get_text(stmt, 3);
        o.side = order_side_from_string(get_text(stmt, 4));
        o.type = order_type_from_string(get_text(stmt, 5));
        o.price = get_double(stmt, 6);
        o.qty = get_double(stmt, 7);
        o.status = order_status_from_string(get_text(stmt, 8));
        o.created_at = get_int64(stmt, 9);
        o.updated_at = get_int64(stmt, 10);
        o.reason = order_reason_from_string(get_text(stmt, 11));
        o.reason_detail = get_text(stmt, 12);
        result.push_back(o);
    }

    finalize(stmt);
    return result;
}

std::optional<Order> SessionDatabase::get_order(const std::string& order_id) {
    auto stmt = prepare("SELECT * FROM orders WHERE order_id = ?;");
    bind_text(stmt, 1, order_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        finalize(stmt);
        return std::nullopt;
    }

    Order o;
    o.order_id = get_text(stmt, 0);
    o.session_id = get_text(stmt, 1);
    o.venue = get_text(stmt, 2);
    o.instrument = get_text(stmt, 3);
    o.side = order_side_from_string(get_text(stmt, 4));
    o.type = order_type_from_string(get_text(stmt, 5));
    o.price = get_double(stmt, 6);
    o.qty = get_double(stmt, 7);
    o.status = order_status_from_string(get_text(stmt, 8));
    o.created_at = get_int64(stmt, 9);
    o.updated_at = get_int64(stmt, 10);
    o.reason = order_reason_from_string(get_text(stmt, 11));
    o.reason_detail = get_text(stmt, 12);

    finalize(stmt);
    return o;
}

// ============================================================================
// FILL OPERATIONS
// ============================================================================

void SessionDatabase::insert_fill(const Fill& fill) {
    auto stmt = prepare(R"(
        INSERT INTO fills (
            fill_id, order_id, session_id, venue, instrument, side,
            price, qty, fee, timestamp, slippage_bps, latency_ms
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )");

    std::string id = fill.fill_id.empty() ? generate_uuid() : fill.fill_id;

    bind_text(stmt, 1, id);
    bind_text(stmt, 2, fill.order_id);
    bind_text(stmt, 3, fill.session_id);
    bind_text(stmt, 4, fill.venue);
    bind_text(stmt, 5, fill.instrument);
    bind_text(stmt, 6, to_string(fill.side));
    bind_double(stmt, 7, fill.price);
    bind_double(stmt, 8, fill.qty);
    bind_double(stmt, 9, fill.fee);
    bind_int64(stmt, 10, fill.timestamp ? fill.timestamp : now_micros());
    bind_double(stmt, 11, fill.slippage_bps);
    bind_int64(stmt, 12, fill.latency_ms);

    sqlite3_step(stmt);
    finalize(stmt);
}

std::vector<Fill> SessionDatabase::get_fills_for_session(const std::string& session_id) {
    auto stmt = prepare(
        "SELECT * FROM fills WHERE session_id = ? ORDER BY timestamp;"
    );
    bind_text(stmt, 1, session_id);

    std::vector<Fill> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Fill f;
        f.fill_id = get_text(stmt, 0);
        f.order_id = get_text(stmt, 1);
        f.session_id = get_text(stmt, 2);
        f.venue = get_text(stmt, 3);
        f.instrument = get_text(stmt, 4);
        f.side = order_side_from_string(get_text(stmt, 5));
        f.price = get_double(stmt, 6);
        f.qty = get_double(stmt, 7);
        f.fee = get_double(stmt, 8);
        f.timestamp = get_int64(stmt, 9);
        f.slippage_bps = get_double(stmt, 10);
        f.latency_ms = get_int64(stmt, 11);
        result.push_back(f);
    }

    finalize(stmt);
    return result;
}

std::vector<Fill> SessionDatabase::get_fills_for_order(const std::string& order_id) {
    auto stmt = prepare(
        "SELECT * FROM fills WHERE order_id = ? ORDER BY timestamp;"
    );
    bind_text(stmt, 1, order_id);

    std::vector<Fill> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Fill f;
        f.fill_id = get_text(stmt, 0);
        f.order_id = get_text(stmt, 1);
        f.session_id = get_text(stmt, 2);
        f.venue = get_text(stmt, 3);
        f.instrument = get_text(stmt, 4);
        f.side = order_side_from_string(get_text(stmt, 5));
        f.price = get_double(stmt, 6);
        f.qty = get_double(stmt, 7);
        f.fee = get_double(stmt, 8);
        f.timestamp = get_int64(stmt, 9);
        f.slippage_bps = get_double(stmt, 10);
        f.latency_ms = get_int64(stmt, 11);
        result.push_back(f);
    }

    finalize(stmt);
    return result;
}

// ============================================================================
// POSITION OPERATIONS
// ============================================================================

void SessionDatabase::upsert_position(const Position& pos) {
    auto stmt = prepare(R"(
        INSERT INTO positions (
            position_id, session_id, venue, instrument, qty, avg_price,
            mark_price, unrealized_pnl, margin_used, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(session_id, venue, instrument) DO UPDATE SET
            qty = excluded.qty,
            avg_price = excluded.avg_price,
            mark_price = excluded.mark_price,
            unrealized_pnl = excluded.unrealized_pnl,
            margin_used = excluded.margin_used,
            updated_at = excluded.updated_at;
    )");

    std::string id = pos.position_id.empty() ? generate_uuid() : pos.position_id;

    bind_text(stmt, 1, id);
    bind_text(stmt, 2, pos.session_id);
    bind_text(stmt, 3, pos.venue);
    bind_text(stmt, 4, pos.instrument);
    bind_double(stmt, 5, pos.qty);
    bind_double(stmt, 6, pos.avg_price);
    bind_double(stmt, 7, pos.mark_price);
    bind_double(stmt, 8, pos.unrealized_pnl);
    bind_double(stmt, 9, pos.margin_used);
    bind_int64(stmt, 10, now_micros());

    sqlite3_step(stmt);
    finalize(stmt);
}

std::vector<Position> SessionDatabase::get_positions_for_session(const std::string& session_id) {
    auto stmt = prepare(
        "SELECT * FROM positions WHERE session_id = ?;"
    );
    bind_text(stmt, 1, session_id);

    std::vector<Position> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Position p;
        p.position_id = get_text(stmt, 0);
        p.session_id = get_text(stmt, 1);
        p.venue = get_text(stmt, 2);
        p.instrument = get_text(stmt, 3);
        p.qty = get_double(stmt, 4);
        p.avg_price = get_double(stmt, 5);
        p.mark_price = get_double(stmt, 6);
        p.unrealized_pnl = get_double(stmt, 7);
        p.margin_used = get_double(stmt, 8);
        p.updated_at = get_int64(stmt, 9);
        result.push_back(p);
    }

    finalize(stmt);
    return result;
}

std::optional<Position> SessionDatabase::get_position(
    const std::string& session_id,
    const std::string& venue,
    const std::string& instrument
) {
    auto stmt = prepare(
        "SELECT * FROM positions WHERE session_id = ? AND venue = ? AND instrument = ?;"
    );
    bind_text(stmt, 1, session_id);
    bind_text(stmt, 2, venue);
    bind_text(stmt, 3, instrument);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        finalize(stmt);
        return std::nullopt;
    }

    Position p;
    p.position_id = get_text(stmt, 0);
    p.session_id = get_text(stmt, 1);
    p.venue = get_text(stmt, 2);
    p.instrument = get_text(stmt, 3);
    p.qty = get_double(stmt, 4);
    p.avg_price = get_double(stmt, 5);
    p.mark_price = get_double(stmt, 6);
    p.unrealized_pnl = get_double(stmt, 7);
    p.margin_used = get_double(stmt, 8);
    p.updated_at = get_int64(stmt, 9);

    finalize(stmt);
    return p;
}

void SessionDatabase::clear_positions_for_session(const std::string& session_id) {
    auto stmt = prepare("DELETE FROM positions WHERE session_id = ?;");
    bind_text(stmt, 1, session_id);
    sqlite3_step(stmt);
    finalize(stmt);
}

// ============================================================================
// FUNDING EVENT OPERATIONS
// ============================================================================

void SessionDatabase::insert_funding_event(const FundingEvent& event) {
    auto stmt = prepare(R"(
        INSERT INTO funding_events (
            funding_event_id, session_id, venue, instrument, funding_rate,
            position_qty, notional, payment_amount, timestamp, next_funding_time
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )");

    std::string id = event.funding_event_id.empty() ? generate_uuid() : event.funding_event_id;

    bind_text(stmt, 1, id);
    bind_text(stmt, 2, event.session_id);
    bind_text(stmt, 3, event.venue);
    bind_text(stmt, 4, event.instrument);
    bind_double(stmt, 5, event.funding_rate);
    bind_double(stmt, 6, event.position_qty);
    bind_double(stmt, 7, event.notional);
    bind_double(stmt, 8, event.payment_amount);
    bind_int64(stmt, 9, event.timestamp ? event.timestamp : now_micros());
    bind_int64(stmt, 10, event.next_funding_time);

    sqlite3_step(stmt);
    finalize(stmt);
}

std::vector<FundingEvent> SessionDatabase::get_funding_events_for_session(
    const std::string& session_id
) {
    auto stmt = prepare(
        "SELECT * FROM funding_events WHERE session_id = ? ORDER BY timestamp;"
    );
    bind_text(stmt, 1, session_id);

    std::vector<FundingEvent> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FundingEvent e;
        e.funding_event_id = get_text(stmt, 0);
        e.session_id = get_text(stmt, 1);
        e.venue = get_text(stmt, 2);
        e.instrument = get_text(stmt, 3);
        e.funding_rate = get_double(stmt, 4);
        e.position_qty = get_double(stmt, 5);
        e.notional = get_double(stmt, 6);
        e.payment_amount = get_double(stmt, 7);
        e.timestamp = get_int64(stmt, 8);
        e.next_funding_time = get_int64(stmt, 9);
        result.push_back(e);
    }

    finalize(stmt);
    return result;
}

double SessionDatabase::get_total_funding_for_session(const std::string& session_id) {
    auto stmt = prepare(
        "SELECT COALESCE(SUM(payment_amount), 0) FROM funding_events WHERE session_id = ?;"
    );
    bind_text(stmt, 1, session_id);

    double total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = get_double(stmt, 0);
    }

    finalize(stmt);
    return total;
}

// ============================================================================
// PNL SNAPSHOT OPERATIONS
// ============================================================================

void SessionDatabase::insert_pnl_snapshot(const PnlSnapshot& snap) {
    auto stmt = prepare(R"(
        INSERT INTO pnl_snapshots (
            snapshot_id, session_id, timestamp, cash_balance, equity,
            unrealized_pnl, realized_pnl, pnl_funding, pnl_fees, pnl_basis,
            exposure_json, leverage, drawdown, high_water_mark
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )");

    std::string id = snap.snapshot_id.empty() ? generate_uuid() : snap.snapshot_id;

    bind_text(stmt, 1, id);
    bind_text(stmt, 2, snap.session_id);
    bind_int64(stmt, 3, snap.timestamp ? snap.timestamp : now_micros());
    bind_double(stmt, 4, snap.cash_balance);
    bind_double(stmt, 5, snap.equity);
    bind_double(stmt, 6, snap.unrealized_pnl);
    bind_double(stmt, 7, snap.realized_pnl);
    bind_double(stmt, 8, snap.pnl_funding);
    bind_double(stmt, 9, snap.pnl_fees);
    bind_double(stmt, 10, snap.pnl_basis);
    bind_text(stmt, 11, snap.exposure_json);
    bind_double(stmt, 12, snap.leverage);
    bind_double(stmt, 13, snap.drawdown);
    bind_double(stmt, 14, snap.high_water_mark);

    sqlite3_step(stmt);
    finalize(stmt);
}

std::vector<PnlSnapshot> SessionDatabase::get_pnl_snapshots_for_session(
    const std::string& session_id
) {
    auto stmt = prepare(
        "SELECT * FROM pnl_snapshots WHERE session_id = ? ORDER BY timestamp;"
    );
    bind_text(stmt, 1, session_id);

    std::vector<PnlSnapshot> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PnlSnapshot s;
        s.snapshot_id = get_text(stmt, 0);
        s.session_id = get_text(stmt, 1);
        s.timestamp = get_int64(stmt, 2);
        s.cash_balance = get_double(stmt, 3);
        s.equity = get_double(stmt, 4);
        s.unrealized_pnl = get_double(stmt, 5);
        s.realized_pnl = get_double(stmt, 6);
        s.pnl_funding = get_double(stmt, 7);
        s.pnl_fees = get_double(stmt, 8);
        s.pnl_basis = get_double(stmt, 9);
        s.exposure_json = get_text(stmt, 10);
        s.leverage = get_double(stmt, 11);
        s.drawdown = get_double(stmt, 12);
        s.high_water_mark = get_double(stmt, 13);
        result.push_back(s);
    }

    finalize(stmt);
    return result;
}

std::optional<PnlSnapshot> SessionDatabase::get_latest_pnl_snapshot(
    const std::string& session_id
) {
    auto stmt = prepare(
        "SELECT * FROM pnl_snapshots WHERE session_id = ? ORDER BY timestamp DESC LIMIT 1;"
    );
    bind_text(stmt, 1, session_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        finalize(stmt);
        return std::nullopt;
    }

    PnlSnapshot s;
    s.snapshot_id = get_text(stmt, 0);
    s.session_id = get_text(stmt, 1);
    s.timestamp = get_int64(stmt, 2);
    s.cash_balance = get_double(stmt, 3);
    s.equity = get_double(stmt, 4);
    s.unrealized_pnl = get_double(stmt, 5);
    s.realized_pnl = get_double(stmt, 6);
    s.pnl_funding = get_double(stmt, 7);
    s.pnl_fees = get_double(stmt, 8);
    s.pnl_basis = get_double(stmt, 9);
    s.exposure_json = get_text(stmt, 10);
    s.leverage = get_double(stmt, 11);
    s.drawdown = get_double(stmt, 12);
    s.high_water_mark = get_double(stmt, 13);

    finalize(stmt);
    return s;
}

// ============================================================================
// KILL EVENT OPERATIONS
// ============================================================================

void SessionDatabase::insert_kill_event(const KillEvent& event) {
    auto stmt = prepare(R"(
        INSERT INTO kill_events (
            kill_event_id, session_id, reason_code, reason_detail,
            timestamp, positions_closed_json, pnl_impact
        ) VALUES (?, ?, ?, ?, ?, ?, ?);
    )");

    std::string id = event.kill_event_id.empty() ? generate_uuid() : event.kill_event_id;

    bind_text(stmt, 1, id);
    bind_text(stmt, 2, event.session_id);
    bind_text(stmt, 3, event.reason_code);
    bind_text(stmt, 4, event.reason_detail);
    bind_int64(stmt, 5, event.timestamp ? event.timestamp : now_micros());
    bind_text(stmt, 6, event.positions_closed_json);
    bind_double(stmt, 7, event.pnl_impact);

    sqlite3_step(stmt);
    finalize(stmt);
}

std::vector<KillEvent> SessionDatabase::get_kill_events_for_session(
    const std::string& session_id
) {
    auto stmt = prepare(
        "SELECT * FROM kill_events WHERE session_id = ? ORDER BY timestamp;"
    );
    bind_text(stmt, 1, session_id);

    std::vector<KillEvent> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        KillEvent e;
        e.kill_event_id = get_text(stmt, 0);
        e.session_id = get_text(stmt, 1);
        e.reason_code = get_text(stmt, 2);
        e.reason_detail = get_text(stmt, 3);
        e.timestamp = get_int64(stmt, 4);
        e.positions_closed_json = get_text(stmt, 5);
        e.pnl_impact = get_double(stmt, 6);
        result.push_back(e);
    }

    finalize(stmt);
    return result;
}

// ============================================================================
// REPORTING
// ============================================================================

SessionSummary SessionDatabase::compute_session_summary(const std::string& session_id) {
    SessionSummary summary;

    auto session = get_session(session_id);
    if (!session) return summary;

    summary.session_id = session->session_id;
    summary.session_name = session->session_name;
    summary.start_time = session->start_time;
    summary.end_time = session->end_time;
    summary.mode = session->mode;
    summary.starting_balance = session->starting_balance;
    summary.ending_balance = session->ending_balance;

    // Get totals from funding
    summary.pnl_funding = get_total_funding_for_session(session_id);

    // Get fills for fee calculation
    auto fills = get_fills_for_session(session_id);
    summary.total_fills = static_cast<int>(fills.size());
    for (const auto& f : fills) {
        summary.pnl_fees -= f.fee;  // Fees are costs
    }

    // Get funding events
    auto funding_events = get_funding_events_for_session(session_id);
    summary.total_funding_events = static_cast<int>(funding_events.size());

    // Get kill events
    auto kill_events = get_kill_events_for_session(session_id);
    summary.total_kill_events = static_cast<int>(kill_events.size());

    // Count hedges
    auto orders = get_orders_for_session(session_id);
    for (const auto& o : orders) {
        if (o.reason == OrderReason::HEDGE) {
            summary.hedge_count++;
        }
    }

    // Get max drawdown from snapshots
    auto snapshots = get_pnl_snapshots_for_session(session_id);
    double max_dd = 0;
    for (const auto& s : snapshots) {
        max_dd = std::max(max_dd, s.drawdown);
    }
    summary.max_drawdown = max_dd;

    // Total PnL
    summary.total_pnl = summary.ending_balance - summary.starting_balance;
    summary.pnl_realized = summary.total_pnl - summary.pnl_funding;

    return summary;
}

std::string SessionDatabase::generate_report(const std::string& session_id) {
    auto summary = compute_session_summary(session_id);

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);

    ss << "\n";
    ss << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    ss << "║                           SESSION REPORT                                      ║\n";
    ss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    ss << "║ Session ID:   " << std::setw(60) << summary.session_id << " ║\n";
    ss << "║ Name:         " << std::setw(60) << summary.session_name << " ║\n";
    ss << "║ Mode:         " << std::setw(60) << to_string(summary.mode) << " ║\n";
    ss << "║ Start:        " << std::setw(60) << format_timestamp(summary.start_time) << " ║\n";
    if (summary.end_time > 0) {
        ss << "║ End:          " << std::setw(60) << format_timestamp(summary.end_time) << " ║\n";
    }
    ss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    ss << "║                              PnL BREAKDOWN                                    ║\n";
    ss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    ss << "║ Starting Balance:   $" << std::setw(15) << summary.starting_balance << "                                ║\n";
    ss << "║ Ending Balance:     $" << std::setw(15) << summary.ending_balance << "                                ║\n";
    ss << "║ ─────────────────────────────────────────────────────────────────────────── ║\n";
    ss << "║ Total PnL:          $" << std::setw(15) << summary.total_pnl
       << (summary.total_pnl >= 0 ? " ✓" : " ✗") << "                              ║\n";
    ss << "║   Funding PnL:      $" << std::setw(15) << summary.pnl_funding << "                                ║\n";
    ss << "║   Fees Paid:        $" << std::setw(15) << summary.pnl_fees << "                                ║\n";
    ss << "║   Realized PnL:     $" << std::setw(15) << summary.pnl_realized << "                                ║\n";
    ss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    ss << "║                             RISK METRICS                                      ║\n";
    ss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    ss << "║ Max Drawdown:       " << std::setw(15) << (summary.max_drawdown * 100) << "%                               ║\n";
    ss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    ss << "║                            ACTIVITY SUMMARY                                   ║\n";
    ss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    ss << "║ Total Fills:        " << std::setw(15) << summary.total_fills << "                                 ║\n";
    ss << "║ Funding Events:     " << std::setw(15) << summary.total_funding_events << "                                 ║\n";
    ss << "║ Hedge Orders:       " << std::setw(15) << summary.hedge_count << "                                 ║\n";
    ss << "║ Kill Events:        " << std::setw(15) << summary.total_kill_events << "                                 ║\n";
    ss << "╚══════════════════════════════════════════════════════════════════════════════╝\n";

    return ss.str();
}

// ============================================================================
// CSV EXPORT
// ============================================================================

void SessionDatabase::export_fills_csv(const std::string& session_id, const std::string& path) {
    auto fills = get_fills_for_session(session_id);

    std::ofstream file(path);
    file << "fill_id,order_id,venue,instrument,side,price,qty,fee,timestamp,slippage_bps,latency_ms\n";

    for (const auto& f : fills) {
        file << f.fill_id << ","
             << f.order_id << ","
             << f.venue << ","
             << f.instrument << ","
             << to_string(f.side) << ","
             << f.price << ","
             << f.qty << ","
             << f.fee << ","
             << f.timestamp << ","
             << f.slippage_bps << ","
             << f.latency_ms << "\n";
    }

    spdlog::info("Exported {} fills to {}", fills.size(), path);
}

void SessionDatabase::export_funding_events_csv(const std::string& session_id, const std::string& path) {
    auto events = get_funding_events_for_session(session_id);

    std::ofstream file(path);
    file << "funding_event_id,venue,instrument,funding_rate,position_qty,notional,payment_amount,timestamp\n";

    for (const auto& e : events) {
        file << e.funding_event_id << ","
             << e.venue << ","
             << e.instrument << ","
             << e.funding_rate << ","
             << e.position_qty << ","
             << e.notional << ","
             << e.payment_amount << ","
             << e.timestamp << "\n";
    }

    spdlog::info("Exported {} funding events to {}", events.size(), path);
}

void SessionDatabase::export_pnl_snapshots_csv(const std::string& session_id, const std::string& path) {
    auto snapshots = get_pnl_snapshots_for_session(session_id);

    std::ofstream file(path);
    file << "timestamp,cash_balance,equity,unrealized_pnl,realized_pnl,pnl_funding,pnl_fees,leverage,drawdown\n";

    for (const auto& s : snapshots) {
        file << s.timestamp << ","
             << s.cash_balance << ","
             << s.equity << ","
             << s.unrealized_pnl << ","
             << s.realized_pnl << ","
             << s.pnl_funding << ","
             << s.pnl_fees << ","
             << s.leverage << ","
             << s.drawdown << "\n";
    }

    spdlog::info("Exported {} PnL snapshots to {}", snapshots.size(), path);
}

}  // namespace arb
