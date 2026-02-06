#pragma once

#include "common/types.hpp"
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace arb {

class PositionManager {
public:
    PositionManager() = default;

    void open_position(const std::string& symbol, const DeltaNeutralPosition& pos);
    void update_spot_fill(const std::string& symbol, Size size, Price price, double fee);
    void update_futures_fill(const std::string& symbol, Size size, Price price, double fee);
    void close_position(const std::string& symbol);

    void record_funding(const std::string& symbol, double amount);
    void mark_to_market(const std::string& symbol, Price spot_price, Price futures_price);

    std::optional<DeltaNeutralPosition> get_position(const std::string& symbol) const;
    std::map<std::string, DeltaNeutralPosition> get_all_positions() const;
    std::vector<std::string> get_open_symbols() const;

    double total_pnl() const;
    double total_funding() const;
    double total_exposure() const;
    int open_position_count() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, DeltaNeutralPosition> positions_;
};

} // namespace arb
