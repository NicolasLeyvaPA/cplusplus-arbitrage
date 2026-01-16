#pragma once

#include <map>
#include <mutex>
#include <vector>
#include "common/types.hpp"

namespace arb {

/**
 * Position in a single token/outcome.
 */
struct Position {
    std::string token_id;
    std::string market_id;
    std::string outcome_name;  // "YES" or "NO"

    Size size{0.0};           // Positive = long
    Price avg_entry_price{0.0};
    Notional cost_basis{0.0};

    // PnL tracking
    Notional realized_pnl{0.0};
    Notional total_fees{0.0};

    // Mark-to-market
    Price last_mark_price{0.0};
    Notional unrealized_pnl{0.0};

    // Timestamps
    Timestamp first_entry;
    Timestamp last_update;

    // Calculate total PnL
    Notional total_pnl() const { return realized_pnl + unrealized_pnl; }

    // Calculate position value at current mark
    Notional market_value() const { return size * last_mark_price; }

    // Is position open?
    bool is_open() const { return size > 0.0001 || size < -0.0001; }
};

/**
 * Position manager tracks all positions and PnL.
 */
class PositionManager {
public:
    PositionManager() = default;

    // Record a fill
    void record_fill(const Fill& fill);

    // Mark positions to market
    void mark_to_market(const std::string& token_id, Price mark_price);

    // Query positions
    std::optional<Position> get_position(const std::string& token_id) const;
    std::vector<Position> get_all_positions() const;
    std::vector<Position> get_open_positions() const;
    std::vector<Position> get_positions_for_market(const std::string& market_id) const;

    // PnL queries
    Notional total_realized_pnl() const;
    Notional total_unrealized_pnl() const;
    Notional total_pnl() const;
    Notional total_fees() const;

    // Exposure
    Notional gross_exposure() const;
    Notional net_exposure() const;

    // Settlement
    void record_settlement(const std::string& market_id, const std::string& winning_token_id);

    // Reset for new day
    void reset_daily_pnl();

    // Snapshot for persistence
    struct Snapshot {
        std::vector<Position> positions;
        Notional realized_pnl;
        Notional total_fees;
        WallClock timestamp;
    };

    Snapshot create_snapshot() const;
    void restore_from_snapshot(const Snapshot& snapshot);

private:
    mutable std::mutex mutex_;
    std::map<std::string, Position> positions_;  // Keyed by token_id

    // Aggregate tracking
    Notional total_realized_pnl_{0.0};
    Notional daily_realized_pnl_{0.0};
    Notional total_fees_{0.0};

    // Helper to update position on fill
    void apply_fill_to_position(Position& pos, const Fill& fill);
};

} // namespace arb
