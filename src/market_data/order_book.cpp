#include "market_data/order_book.hpp"
#include <algorithm>

namespace arb {

OrderBook::OrderBook(const std::string& symbol, int max_levels)
    : symbol_(symbol)
    , max_levels_(max_levels)
    , last_update_(now())
{
}

void OrderBook::update_bid(Price price, Size size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (size <= 0.0) {
        bids_.erase(price);
    } else {
        bids_[price] = size;
    }
    last_update_ = now();
    trim_levels();
}

void OrderBook::update_ask(Price price, Size size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (size <= 0.0) {
        asks_.erase(price);
    } else {
        asks_[price] = size;
    }
    last_update_ = now();
    trim_levels();
}

void OrderBook::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    bids_.clear();
    asks_.clear();
    last_update_ = now();
}

void OrderBook::apply_snapshot(const std::vector<PriceLevel>& bids,
                               const std::vector<PriceLevel>& asks) {
    std::lock_guard<std::mutex> lock(mutex_);

    bids_.clear();
    for (const auto& level : bids) {
        if (level.size > 0.0) {
            bids_[level.price] = level.size;
        }
    }

    asks_.clear();
    for (const auto& level : asks) {
        if (level.size > 0.0) {
            asks_[level.price] = level.size;
        }
    }

    last_update_ = now();
    trim_levels();
}

std::optional<PriceLevel> OrderBook::best_bid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bids_.empty()) return std::nullopt;
    auto it = bids_.begin();
    return PriceLevel{it->first, it->second};
}

std::optional<PriceLevel> OrderBook::best_ask() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (asks_.empty()) return std::nullopt;
    auto it = asks_.begin();
    return PriceLevel{it->first, it->second};
}

Price OrderBook::mid_price() const {
    auto bid = best_bid();
    auto ask = best_ask();
    if (!bid || !ask) return 0.0;
    return (bid->price + ask->price) / 2.0;
}

Price OrderBook::spread() const {
    auto bid = best_bid();
    auto ask = best_ask();
    if (!bid || !ask) return 0.0;
    return ask->price - bid->price;
}

Price OrderBook::spread_bps() const {
    auto mid = mid_price();
    if (mid <= 0.0) return 0.0;
    return (spread() / mid) * 10000.0;
}

std::vector<PriceLevel> OrderBook::top_bids(int n) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PriceLevel> result;
    int count = 0;
    for (const auto& [price, size] : bids_) {
        if (count >= n) break;
        result.push_back({price, size});
        count++;
    }
    return result;
}

std::vector<PriceLevel> OrderBook::top_asks(int n) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PriceLevel> result;
    int count = 0;
    for (const auto& [price, size] : asks_) {
        if (count >= n) break;
        result.push_back({price, size});
        count++;
    }
    return result;
}

Size OrderBook::bid_depth(int levels) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Size total = 0.0;
    int count = 0;
    for (const auto& [price, size] : bids_) {
        if (count >= levels) break;
        total += size;
        count++;
    }
    return total;
}

Size OrderBook::ask_depth(int levels) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Size total = 0.0;
    int count = 0;
    for (const auto& [price, size] : asks_) {
        if (count >= levels) break;
        total += size;
        count++;
    }
    return total;
}

Size OrderBook::total_depth(int levels) const {
    return bid_depth(levels) + ask_depth(levels);
}

Timestamp OrderBook::last_update_time() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_update_;
}

bool OrderBook::is_stale(Duration threshold) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (now() - last_update_) > threshold;
}

void OrderBook::trim_levels() {
    // Already holding lock
    while (static_cast<int>(bids_.size()) > max_levels_) {
        auto it = bids_.end();
        --it;
        bids_.erase(it);
    }
    while (static_cast<int>(asks_.size()) > max_levels_) {
        auto it = asks_.end();
        --it;
        asks_.erase(it);
    }
}

// BinaryMarketBook implementation

BinaryMarketBook::BinaryMarketBook(const std::string& market_id)
    : market_id_(market_id)
    , yes_book_(market_id + "_YES")
    , no_book_(market_id + "_NO")
{
}

double BinaryMarketBook::sum_of_best_asks() const {
    auto yes_ask = yes_book_.best_ask();
    auto no_ask = no_book_.best_ask();

    if (!yes_ask || !no_ask) return 0.0;

    return yes_ask->price + no_ask->price;
}

double BinaryMarketBook::sum_of_best_bids() const {
    auto yes_bid = yes_book_.best_bid();
    auto no_bid = no_book_.best_bid();

    if (!yes_bid || !no_bid) return 0.0;

    return yes_bid->price + no_bid->price;
}

double BinaryMarketBook::yes_implied_probability() const {
    // Mid price as implied probability
    return yes_book_.mid_price();
}

bool BinaryMarketBook::has_liquidity() const {
    return yes_book_.best_bid().has_value() &&
           yes_book_.best_ask().has_value() &&
           no_book_.best_bid().has_value() &&
           no_book_.best_ask().has_value();
}

bool BinaryMarketBook::is_stale(Duration threshold) const {
    return yes_book_.is_stale(threshold) || no_book_.is_stale(threshold);
}

} // namespace arb
