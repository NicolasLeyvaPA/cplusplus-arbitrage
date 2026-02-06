#include "strategy/basis_calculator.hpp"
#include <cmath>

namespace arb {

void BasisCalculator::update(const std::string& symbol, Price spot_price, Price futures_price) {
    if (spot_price <= 0 || futures_price <= 0) return;

    BasisInfo info;
    info.symbol = symbol;
    info.spot_mid = spot_price;
    info.futures_mid = futures_price;
    info.basis = (futures_price - spot_price) / spot_price;
    info.basis_bps = info.basis * 10000.0;
    // Rough annualization assuming 8h funding periods
    info.annualized_basis = info.basis * 3.0 * 365.0 * 100.0;
    info.timestamp_ms = static_cast<uint64_t>(now_ms());

    std::lock_guard<std::mutex> lock(mutex_);
    basis_cache_[symbol] = info;
}

BasisInfo BasisCalculator::get_basis(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = basis_cache_.find(symbol);
    if (it != basis_cache_.end()) return it->second;
    return BasisInfo{};
}

std::map<std::string, BasisInfo> BasisCalculator::get_all_basis() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return basis_cache_;
}

bool BasisCalculator::basis_is_acceptable(const std::string& symbol, double max_deviation_pct) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = basis_cache_.find(symbol);
    if (it == basis_cache_.end()) return false;
    return std::abs(it->second.basis) * 100.0 <= max_deviation_pct;
}

} // namespace arb
