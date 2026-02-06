#pragma once

#include "common/types.hpp"
#include <map>
#include <mutex>

namespace arb {

/**
 * Tracks and computes spot-futures basis for each symbol.
 */
class BasisCalculator {
public:
    void update(const std::string& symbol, Price spot_price, Price futures_price);
    BasisInfo get_basis(const std::string& symbol) const;
    std::map<std::string, BasisInfo> get_all_basis() const;
    bool basis_is_acceptable(const std::string& symbol, double max_deviation_pct) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, BasisInfo> basis_cache_;
};

} // namespace arb
