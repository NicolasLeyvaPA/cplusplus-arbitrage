#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>
#include "common/types.hpp"
#include "execution/order.hpp"
#include "position/position_manager.hpp"

namespace arb {

/**
 * Persistent trade ledger for recording all trading activity.
 * Writes to JSON lines format for easy processing.
 */
class TradeLedger {
public:
    explicit TradeLedger(const std::string& path);
    ~TradeLedger();

    // Record events
    void record_fill(const Fill& fill);
    void record_order(const Order& order);
    void record_signal(const Signal& signal);
    void record_position_snapshot(const Position& position);

    // Generic event recording
    void record_event(const std::string& event_type, const nlohmann::json& data);

    // Query historical data
    std::vector<Fill> get_fills(WallClock start, WallClock end) const;
    std::vector<Order> get_orders(WallClock start, WallClock end) const;

    // Summary statistics
    struct DailySummary {
        WallClock date;
        int trades{0};
        int orders{0};
        Notional volume{0.0};
        Notional pnl{0.0};
        Notional fees{0.0};
        int winning_trades{0};
        int losing_trades{0};
    };

    DailySummary get_daily_summary(WallClock date) const;
    std::vector<DailySummary> get_summary_range(WallClock start, WallClock end) const;

    // Export
    void export_to_csv(const std::string& path, WallClock start, WallClock end) const;

    // Ledger management
    void flush();
    void rotate();  // Rotate to new file if too large
    size_t file_size() const;

private:
    std::string base_path_;
    std::string current_path_;
    std::ofstream file_;
    mutable std::mutex mutex_;

    static constexpr size_t MAX_FILE_SIZE = 100 * 1024 * 1024;  // 100MB

    void open_file();
    void write_line(const nlohmann::json& j);
};

/**
 * State snapshot for restart recovery.
 */
class StateSnapshot {
public:
    explicit StateSnapshot(const std::string& path);

    // Save current state
    void save(
        const std::vector<Order>& open_orders,
        const std::vector<Position>& positions,
        double balance,
        double daily_pnl
    );

    // Load state
    struct State {
        std::vector<Order> open_orders;
        std::vector<Position> positions;
        double balance{0.0};
        double daily_pnl{0.0};
        WallClock timestamp;
        bool valid{false};
    };

    State load() const;

    // Check if snapshot exists and is recent
    bool has_recent_snapshot(Duration max_age) const;

private:
    std::string path_;
    mutable std::mutex mutex_;
};

// JSON serialization for persistence types
void to_json(nlohmann::json& j, const Fill& f);
void from_json(const nlohmann::json& j, Fill& f);

void to_json(nlohmann::json& j, const Order& o);
void from_json(const nlohmann::json& j, Order& o);

void to_json(nlohmann::json& j, const Signal& s);
void from_json(const nlohmann::json& j, Signal& s);

void to_json(nlohmann::json& j, const Position& p);
void from_json(const nlohmann::json& j, Position& p);

} // namespace arb
