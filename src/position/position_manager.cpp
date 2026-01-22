#include "position/position_manager.hpp"
#include <spdlog/spdlog.h>

namespace arb {

void PositionManager::record_fill(const Fill& fill) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = positions_.find(fill.token_id);

    if (it == positions_.end()) {
        // Create new position
        Position pos;
        pos.token_id = fill.token_id;
        pos.market_id = fill.market_id;
        pos.first_entry = now();
        positions_[fill.token_id] = pos;
        it = positions_.find(fill.token_id);
    }

    Position& pos = it->second;
    apply_fill_to_position(pos, fill);

    total_fees_ += fill.fee;

    spdlog::debug("Position updated: {} size={:.2f} avg_price={:.4f} realized_pnl={:.2f}",
                 fill.token_id, pos.size, pos.avg_entry_price, pos.realized_pnl);
}

void PositionManager::apply_fill_to_position(Position& pos, const Fill& fill) {
    double signed_size = (fill.side == Side::BUY) ? fill.size : -fill.size;
    double fill_notional = fill.price * fill.size;

    if ((pos.size >= 0 && fill.side == Side::BUY) ||
        (pos.size <= 0 && fill.side == Side::SELL)) {
        // Adding to position
        double new_size = pos.size + signed_size;
        if (std::abs(new_size) > 0.0001) {
            pos.avg_entry_price = (pos.cost_basis + fill_notional) / std::abs(new_size);
        }
        pos.cost_basis += fill_notional;
        pos.size = new_size;
    } else {
        // Reducing position
        double reduction = std::min(std::abs(signed_size), std::abs(pos.size));
        double realized = reduction * (fill.price - pos.avg_entry_price);

        if (fill.side == Side::SELL) {
            realized = -realized;  // Profit from selling at higher price
        }

        pos.realized_pnl += realized - fill.fee;
        total_realized_pnl_ += realized - fill.fee;
        daily_realized_pnl_ += realized - fill.fee;

        pos.size += signed_size;
        pos.cost_basis = std::abs(pos.size) * pos.avg_entry_price;
    }

    pos.total_fees += fill.fee;
    pos.last_update = now();
}

void PositionManager::mark_to_market(const std::string& token_id, Price mark_price) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = positions_.find(token_id);
    if (it == positions_.end()) return;

    Position& pos = it->second;
    pos.last_mark_price = mark_price;

    if (std::abs(pos.size) > 0.0001) {
        pos.unrealized_pnl = pos.size * (mark_price - pos.avg_entry_price);
    } else {
        pos.unrealized_pnl = 0.0;
    }
}

std::optional<Position> PositionManager::get_position(const std::string& token_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = positions_.find(token_id);
    if (it != positions_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<Position> PositionManager::get_all_positions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Position> result;
    for (const auto& [id, pos] : positions_) {
        result.push_back(pos);
    }
    return result;
}

std::vector<Position> PositionManager::get_open_positions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Position> result;
    for (const auto& [id, pos] : positions_) {
        if (pos.is_open()) {
            result.push_back(pos);
        }
    }
    return result;
}

std::vector<Position> PositionManager::get_positions_for_market(const std::string& market_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Position> result;
    for (const auto& [id, pos] : positions_) {
        if (pos.market_id == market_id) {
            result.push_back(pos);
        }
    }
    return result;
}

Notional PositionManager::total_realized_pnl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_realized_pnl_;
}

Notional PositionManager::total_unrealized_pnl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Notional total = 0.0;
    for (const auto& [id, pos] : positions_) {
        total += pos.unrealized_pnl;
    }
    return total;
}

Notional PositionManager::total_pnl() const {
    return total_realized_pnl() + total_unrealized_pnl();
}

Notional PositionManager::total_fees() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_fees_;
}

Notional PositionManager::gross_exposure() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Notional total = 0.0;
    for (const auto& [id, pos] : positions_) {
        total += std::abs(pos.market_value());
    }
    return total;
}

Notional PositionManager::net_exposure() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Notional total = 0.0;
    for (const auto& [id, pos] : positions_) {
        total += pos.market_value();
    }
    return total;
}

void PositionManager::record_settlement(const std::string& market_id,
                                         const std::string& winning_token_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [id, pos] : positions_) {
        if (pos.market_id == market_id) {
            if (id == winning_token_id) {
                // Position settles to $1 per share
                double pnl = pos.size * (1.0 - pos.avg_entry_price) - pos.total_fees;
                pos.realized_pnl += pnl;
                total_realized_pnl_ += pnl;
            } else {
                // Position settles to $0
                double pnl = -pos.cost_basis - pos.total_fees;
                pos.realized_pnl += pnl;
                total_realized_pnl_ += pnl;
            }

            pos.size = 0.0;
            pos.cost_basis = 0.0;
            pos.unrealized_pnl = 0.0;
            pos.last_update = now();

            spdlog::info("Settlement recorded: {} winner={} pnl={:.2f}",
                        market_id, winning_token_id, pos.realized_pnl);
        }
    }
}

void PositionManager::reset_daily_pnl() {
    std::lock_guard<std::mutex> lock(mutex_);
    daily_realized_pnl_ = 0.0;
}

PositionManager::Snapshot PositionManager::create_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Snapshot snap;
    for (const auto& [id, pos] : positions_) {
        snap.positions.push_back(pos);
    }
    snap.realized_pnl = total_realized_pnl_;
    snap.total_fees = total_fees_;
    snap.timestamp = wall_now();

    return snap;
}

void PositionManager::restore_from_snapshot(const Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);

    positions_.clear();
    for (const auto& pos : snapshot.positions) {
        positions_[pos.token_id] = pos;
    }

    total_realized_pnl_ = snapshot.realized_pnl;
    total_fees_ = snapshot.total_fees;

    spdlog::info("Restored {} positions from snapshot", positions_.size());
}

} // namespace arb
