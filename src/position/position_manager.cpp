#include "position/position_manager.hpp"
#include <spdlog/spdlog.h>
#include <cmath>

namespace arb {

void PositionManager::open_position(const std::string& symbol, const DeltaNeutralPosition& pos) {
    std::lock_guard<std::mutex> lock(mutex_);
    positions_[symbol] = pos;
    spdlog::info("Position opened: {} spot={:.6f} futures={:.6f}",
                 symbol, pos.spot_size, pos.futures_size);
}

void PositionManager::update_spot_fill(const std::string& symbol, Size size, Price price, double fee) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& pos = positions_[symbol];
    pos.symbol = symbol;
    pos.spot_size += size;
    pos.spot_notional += size * price;
    if (pos.spot_size != 0) {
        pos.spot_avg_entry = pos.spot_notional / pos.spot_size;
    }
    pos.total_fees += fee;
    pos.last_update_ms = static_cast<uint64_t>(now_ms());
}

void PositionManager::update_futures_fill(const std::string& symbol, Size size, Price price, double fee) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& pos = positions_[symbol];
    pos.symbol = symbol;
    pos.futures_size += size;
    pos.futures_notional += size * price;
    if (pos.futures_size != 0) {
        pos.futures_avg_entry = std::abs(pos.futures_notional / pos.futures_size);
    }
    pos.total_fees += fee;
    pos.last_update_ms = static_cast<uint64_t>(now_ms());
}

void PositionManager::close_position(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = positions_.find(symbol);
    if (it != positions_.end()) {
        spdlog::info("Position closed: {} PnL={:.2f}", symbol, it->second.total_pnl());
        positions_.erase(it);
    }
}

void PositionManager::record_funding(const std::string& symbol, double amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = positions_.find(symbol);
    if (it != positions_.end()) {
        it->second.funding_collected += amount;
        it->second.funding_periods++;
        spdlog::info("Funding received: {} ${:.4f} (total: ${:.4f}, periods: {})",
                     symbol, amount, it->second.funding_collected, it->second.funding_periods);
    }
}

void PositionManager::mark_to_market(const std::string& symbol, Price spot_price, Price futures_price) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = positions_.find(symbol);
    if (it == positions_.end()) return;

    auto& pos = it->second;
    double spot_pnl = pos.spot_size * (spot_price - pos.spot_avg_entry);
    double futures_pnl = pos.futures_size * (futures_price - pos.futures_avg_entry);
    pos.unrealized_pnl = spot_pnl + futures_pnl;
    pos.last_update_ms = static_cast<uint64_t>(now_ms());
}

std::optional<DeltaNeutralPosition> PositionManager::get_position(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = positions_.find(symbol);
    if (it != positions_.end()) return it->second;
    return std::nullopt;
}

std::map<std::string, DeltaNeutralPosition> PositionManager::get_all_positions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return positions_;
}

std::vector<std::string> PositionManager::get_open_symbols() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> symbols;
    for (const auto& [sym, pos] : positions_) {
        if (pos.is_open()) symbols.push_back(sym);
    }
    return symbols;
}

double PositionManager::total_pnl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double total = 0;
    for (const auto& [_, pos] : positions_) {
        total += pos.total_pnl();
    }
    return total;
}

double PositionManager::total_funding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double total = 0;
    for (const auto& [_, pos] : positions_) {
        total += pos.funding_collected;
    }
    return total;
}

double PositionManager::total_exposure() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double total = 0;
    for (const auto& [_, pos] : positions_) {
        total += pos.total_notional();
    }
    return total;
}

int PositionManager::open_position_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& [_, pos] : positions_) {
        if (pos.is_open()) count++;
    }
    return count;
}

} // namespace arb
