#pragma once

#include <map>
#include <vector>
#include <mutex>
#include <optional>
#include "common/types.hpp"

namespace arb {

/**
 * Thread-safe order book implementation maintaining sorted price levels.
 * Supports both Polymarket (binary outcomes) and general use.
 */
class OrderBook {
public:
    explicit OrderBook(const std::string& symbol, int max_levels = 10);

    // Update methods
    void update_bid(Price price, Size size);
    void update_ask(Price price, Size size);
    void clear();

    // Full snapshot update
    void apply_snapshot(const std::vector<PriceLevel>& bids,
                       const std::vector<PriceLevel>& asks);

    // Query methods (thread-safe)
    std::optional<PriceLevel> best_bid() const;
    std::optional<PriceLevel> best_ask() const;
    Price mid_price() const;
    Price spread() const;
    Price spread_bps() const;  // Spread in basis points

    // Get top N levels
    std::vector<PriceLevel> top_bids(int n) const;
    std::vector<PriceLevel> top_asks(int n) const;

    // Liquidity queries
    Size bid_depth(int levels) const;
    Size ask_depth(int levels) const;
    Size total_depth(int levels) const;

    // Staleness check
    Timestamp last_update_time() const;
    bool is_stale(Duration threshold) const;

    // Symbol accessor
    const std::string& symbol() const { return symbol_; }

    // Sequence number for ordering
    void set_sequence(uint64_t seq) { sequence_ = seq; }
    uint64_t sequence() const { return sequence_; }

private:
    std::string symbol_;
    int max_levels_;
    uint64_t sequence_{0};
    Timestamp last_update_;

    // Bids sorted descending (highest first)
    std::map<Price, Size, std::greater<Price>> bids_;
    // Asks sorted ascending (lowest first)
    std::map<Price, Size> asks_;

    mutable std::mutex mutex_;

    void trim_levels();
};

/**
 * Binary market book that tracks both YES and NO outcomes.
 */
class BinaryMarketBook {
public:
    explicit BinaryMarketBook(const std::string& market_id);

    OrderBook& yes_book() { return yes_book_; }
    OrderBook& no_book() { return no_book_; }
    const OrderBook& yes_book() const { return yes_book_; }
    const OrderBook& no_book() const { return no_book_; }

    // Sum of best asks (for underpricing detection)
    double sum_of_best_asks() const;

    // Sum of best bids
    double sum_of_best_bids() const;

    // Implied probability from mid prices
    double yes_implied_probability() const;

    // Check if both sides have liquidity
    bool has_liquidity() const;

    // Staleness across both books
    bool is_stale(Duration threshold) const;

    const std::string& market_id() const { return market_id_; }

private:
    std::string market_id_;
    OrderBook yes_book_;
    OrderBook no_book_;
};

} // namespace arb
