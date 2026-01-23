#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <chrono>
#include <memory>
#include <optional>
#include <random>
#include <cmath>

#include "persistence/session_database.hpp"

namespace arb {

// ============================================================================
// FUNDING SETTLEMENT ENGINE
//
// Simulates realistic funding payments for perpetual futures positions.
// In demo mode: uses historical funding rates with realistic timing
// In live mode: records actual funding payments from exchanges
//
// Key concepts:
// - Funding payments occur at fixed intervals (typically every 8 hours)
// - Payment = position_notional * funding_rate
// - Positive funding rate: longs pay shorts
// - Negative funding rate: shorts pay longs
// ============================================================================

struct FundingRate {
    std::string venue;
    std::string instrument;
    double rate{0};                  // Funding rate for current period
    double predicted_rate{0};        // Predicted rate for next period
    int64_t current_funding_time{0}; // When current rate was applied
    int64_t next_funding_time{0};    // When next funding occurs
    double mark_price{0};
    double index_price{0};
};

struct DemoFillConfig {
    // Fill probability model
    double base_fill_probability{0.85};   // 85% base fill rate
    double spread_penalty_per_bps{0.01};  // -1% fill prob per bp of spread
    double max_fill_probability{0.98};
    double min_fill_probability{0.40};    // Adversarial minimum

    // Slippage model (adverse selection)
    double slippage_mean_bps{2.0};        // Average slippage
    double slippage_stddev_bps{3.0};      // Slippage volatility
    double slippage_skew{0.3};            // Right-skewed (mostly adverse)

    // Latency model
    int64_t latency_mean_ms{150};
    int64_t latency_stddev_ms{50};

    // Fee assumptions
    double maker_fee_bps{2.0};
    double taker_fee_bps{5.0};
};

struct SimulatedFill {
    bool filled{false};
    double fill_price{0};
    double fill_qty{0};
    double fee{0};
    double slippage_bps{0};
    int64_t latency_ms{0};
    std::string rejection_reason;
};

class FundingSettlementEngine {
public:
    explicit FundingSettlementEngine(
        std::shared_ptr<SessionDatabase> db,
        const std::string& session_id,
        const DemoFillConfig& config = DemoFillConfig()
    );

    // ========================================================================
    // Funding Rate Management
    // ========================================================================

    // Update funding rate for a venue/instrument
    void update_funding_rate(const FundingRate& rate);

    // Get current funding rate
    std::optional<FundingRate> get_funding_rate(
        const std::string& venue,
        const std::string& instrument
    ) const;

    // Get all current funding rates
    std::map<std::string, FundingRate> get_all_funding_rates() const;

    // ========================================================================
    // Funding Settlement
    // ========================================================================

    // Check if funding settlement is due (call periodically)
    // Returns list of venues that need funding settlement
    std::vector<std::string> check_funding_due();

    // Settle funding for a specific venue/instrument
    // Returns the funding payment amount (positive = received, negative = paid)
    double settle_funding(
        const std::string& venue,
        const std::string& instrument,
        double position_qty,
        double mark_price
    );

    // Settle all pending funding across all positions
    // Returns total funding payment
    double settle_all_funding(const std::vector<Position>& positions);

    // ========================================================================
    // Demo Mode: Realistic Fill Simulation
    // ========================================================================

    // Simulate order fill with realistic execution model
    SimulatedFill simulate_fill(
        const std::string& venue,
        const std::string& instrument,
        OrderSide side,
        double qty,
        double limit_price,       // 0 for market order
        double current_bid,
        double current_ask,
        double bid_qty,
        double ask_qty
    );

    // ========================================================================
    // Callbacks
    // ========================================================================

    using FundingCallback = std::function<void(const FundingEvent&)>;
    using FillCallback = std::function<void(const Fill&)>;

    void set_funding_callback(FundingCallback cb) { on_funding_ = std::move(cb); }
    void set_fill_callback(FillCallback cb) { on_fill_ = std::move(cb); }

    // ========================================================================
    // Statistics
    // ========================================================================

    struct Stats {
        int funding_settlements{0};
        double total_funding_received{0};
        double total_funding_paid{0};
        double net_funding{0};

        int orders_simulated{0};
        int orders_filled{0};
        int orders_rejected{0};
        double total_slippage_cost{0};
        double total_fees_paid{0};
    };

    Stats stats() const { return stats_; }

private:
    std::shared_ptr<SessionDatabase> db_;
    std::string session_id_;
    DemoFillConfig config_;
    Stats stats_;

    // Current funding rates by venue:instrument
    std::map<std::string, FundingRate> funding_rates_;

    // Last settlement time by venue:instrument
    std::map<std::string, int64_t> last_settlement_time_;

    // Random number generation
    std::mt19937 rng_;
    std::normal_distribution<double> slippage_dist_;
    std::normal_distribution<double> latency_dist_;
    std::uniform_real_distribution<double> fill_prob_dist_;

    // Callbacks
    FundingCallback on_funding_;
    FillCallback on_fill_;

    // Helper functions
    std::string make_key(const std::string& venue, const std::string& instrument) const;
    double calculate_fill_probability(double spread_bps, double order_size_ratio) const;
    double calculate_slippage(OrderSide side, double volatility) const;
    int64_t calculate_latency() const;
};

// ============================================================================
// Implementation
// ============================================================================

inline FundingSettlementEngine::FundingSettlementEngine(
    std::shared_ptr<SessionDatabase> db,
    const std::string& session_id,
    const DemoFillConfig& config
)
    : db_(std::move(db))
    , session_id_(session_id)
    , config_(config)
    , rng_(std::random_device{}())
    , slippage_dist_(config_.slippage_mean_bps, config_.slippage_stddev_bps)
    , latency_dist_(static_cast<double>(config_.latency_mean_ms),
                    static_cast<double>(config_.latency_stddev_ms))
    , fill_prob_dist_(0.0, 1.0)
{
}

inline std::string FundingSettlementEngine::make_key(
    const std::string& venue,
    const std::string& instrument
) const {
    return venue + ":" + instrument;
}

inline void FundingSettlementEngine::update_funding_rate(const FundingRate& rate) {
    std::string key = make_key(rate.venue, rate.instrument);
    funding_rates_[key] = rate;
}

inline std::optional<FundingRate> FundingSettlementEngine::get_funding_rate(
    const std::string& venue,
    const std::string& instrument
) const {
    std::string key = make_key(venue, instrument);
    auto it = funding_rates_.find(key);
    if (it == funding_rates_.end()) {
        return std::nullopt;
    }
    return it->second;
}

inline std::map<std::string, FundingRate> FundingSettlementEngine::get_all_funding_rates() const {
    return funding_rates_;
}

inline std::vector<std::string> FundingSettlementEngine::check_funding_due() {
    std::vector<std::string> due;
    int64_t now = now_micros();

    for (const auto& [key, rate] : funding_rates_) {
        if (rate.next_funding_time > 0 && now >= rate.next_funding_time * 1000000) {
            // Check if we haven't already settled for this period
            auto it = last_settlement_time_.find(key);
            if (it == last_settlement_time_.end() ||
                it->second < rate.current_funding_time * 1000000) {
                due.push_back(key);
            }
        }
    }

    return due;
}

inline double FundingSettlementEngine::settle_funding(
    const std::string& venue,
    const std::string& instrument,
    double position_qty,
    double mark_price
) {
    std::string key = make_key(venue, instrument);
    auto it = funding_rates_.find(key);
    if (it == funding_rates_.end()) {
        return 0;
    }

    const FundingRate& rate = it->second;
    double notional = std::abs(position_qty) * mark_price;

    // Funding payment calculation:
    // - Long position: pays funding when rate > 0, receives when rate < 0
    // - Short position: receives funding when rate > 0, pays when rate < 0
    // Payment = -position_qty * notional * funding_rate
    // (negative position = short, so short with positive rate = positive payment)
    double payment = -position_qty * mark_price * rate.rate;

    // Record the funding event
    FundingEvent event;
    event.session_id = session_id_;
    event.venue = venue;
    event.instrument = instrument;
    event.funding_rate = rate.rate;
    event.position_qty = position_qty;
    event.notional = notional;
    event.payment_amount = payment;
    event.timestamp = now_micros();
    event.next_funding_time = rate.next_funding_time;

    if (db_) {
        db_->insert_funding_event(event);
    }

    // Update last settlement time
    last_settlement_time_[key] = now_micros();

    // Update stats
    stats_.funding_settlements++;
    if (payment > 0) {
        stats_.total_funding_received += payment;
    } else {
        stats_.total_funding_paid += std::abs(payment);
    }
    stats_.net_funding += payment;

    // Callback
    if (on_funding_) {
        on_funding_(event);
    }

    return payment;
}

inline double FundingSettlementEngine::settle_all_funding(
    const std::vector<Position>& positions
) {
    auto due_venues = check_funding_due();
    double total_payment = 0;

    for (const auto& key : due_venues) {
        // Parse venue:instrument
        size_t colon = key.find(':');
        if (colon == std::string::npos) continue;

        std::string venue = key.substr(0, colon);
        std::string instrument = key.substr(colon + 1);

        // Find matching position
        for (const auto& pos : positions) {
            if (pos.venue == venue && pos.instrument == instrument && pos.qty != 0) {
                auto rate_opt = get_funding_rate(venue, instrument);
                double mark_price = rate_opt ? rate_opt->mark_price : pos.mark_price;
                if (mark_price <= 0) mark_price = pos.avg_price;

                double payment = settle_funding(venue, instrument, pos.qty, mark_price);
                total_payment += payment;
            }
        }
    }

    return total_payment;
}

inline double FundingSettlementEngine::calculate_fill_probability(
    double spread_bps,
    double order_size_ratio
) const {
    // Base probability minus spread penalty
    double prob = config_.base_fill_probability
                - spread_bps * config_.spread_penalty_per_bps;

    // Larger orders relative to book depth are less likely to fill completely
    prob -= order_size_ratio * 0.2;  // -20% for each 100% of book depth

    // Clamp to configured range
    prob = std::max(config_.min_fill_probability, prob);
    prob = std::min(config_.max_fill_probability, prob);

    return prob;
}

inline double FundingSettlementEngine::calculate_slippage(
    OrderSide side,
    double volatility
) const {
    // Base slippage from normal distribution
    double slippage = const_cast<std::normal_distribution<double>&>(slippage_dist_)(
        const_cast<std::mt19937&>(rng_)
    );

    // Apply skew (adverse selection - slippage tends to be against us)
    slippage = std::abs(slippage) + config_.slippage_skew * volatility;

    // Slippage is always adverse
    return slippage;
}

inline int64_t FundingSettlementEngine::calculate_latency() const {
    double latency = const_cast<std::normal_distribution<double>&>(latency_dist_)(
        const_cast<std::mt19937&>(rng_)
    );
    return std::max(10L, static_cast<int64_t>(latency));
}

inline SimulatedFill FundingSettlementEngine::simulate_fill(
    const std::string& venue,
    const std::string& instrument,
    OrderSide side,
    double qty,
    double limit_price,
    double current_bid,
    double current_ask,
    double bid_qty,
    double ask_qty
) {
    SimulatedFill result;
    stats_.orders_simulated++;

    // Calculate spread
    double spread = current_ask - current_bid;
    double mid = (current_bid + current_ask) / 2.0;
    double spread_bps = (spread / mid) * 10000;

    // Calculate order size ratio (how much of the book we're taking)
    double book_qty = (side == OrderSide::BUY) ? ask_qty : bid_qty;
    double size_ratio = (book_qty > 0) ? qty / book_qty : 1.0;

    // Determine fill probability
    double fill_prob = calculate_fill_probability(spread_bps, size_ratio);

    // Roll for fill
    double roll = fill_prob_dist_(rng_);
    if (roll > fill_prob) {
        result.filled = false;
        result.rejection_reason = "Simulated non-fill (adversarial model)";
        stats_.orders_rejected++;
        return result;
    }

    // Calculate slippage
    double volatility = spread_bps * 0.5;  // Use spread as proxy for volatility
    result.slippage_bps = calculate_slippage(side, volatility);

    // Calculate fill price
    double reference_price = (side == OrderSide::BUY) ? current_ask : current_bid;
    double slippage_amount = reference_price * (result.slippage_bps / 10000.0);

    if (side == OrderSide::BUY) {
        result.fill_price = reference_price + slippage_amount;  // Pay more
    } else {
        result.fill_price = reference_price - slippage_amount;  // Receive less
    }

    // Check against limit price
    if (limit_price > 0) {
        if (side == OrderSide::BUY && result.fill_price > limit_price) {
            result.filled = false;
            result.rejection_reason = "Limit price exceeded";
            stats_.orders_rejected++;
            return result;
        }
        if (side == OrderSide::SELL && result.fill_price < limit_price) {
            result.filled = false;
            result.rejection_reason = "Limit price not met";
            stats_.orders_rejected++;
            return result;
        }
    }

    // Fill succeeded
    result.filled = true;
    result.fill_qty = qty;
    result.latency_ms = calculate_latency();

    // Calculate fee (assume taker since we're crossing spread)
    double notional = result.fill_price * qty;
    result.fee = notional * (config_.taker_fee_bps / 10000.0);

    stats_.orders_filled++;
    stats_.total_slippage_cost += notional * (result.slippage_bps / 10000.0);
    stats_.total_fees_paid += result.fee;

    // Record fill in database
    if (db_) {
        Fill fill;
        fill.session_id = session_id_;
        fill.venue = venue;
        fill.instrument = instrument;
        fill.side = side;
        fill.price = result.fill_price;
        fill.qty = result.fill_qty;
        fill.fee = result.fee;
        fill.slippage_bps = result.slippage_bps;
        fill.latency_ms = result.latency_ms;
        fill.timestamp = now_micros();

        // Note: order_id should be set by caller before recording
        // db_->insert_fill(fill);

        if (on_fill_) {
            on_fill_(fill);
        }
    }

    return result;
}

}  // namespace arb
