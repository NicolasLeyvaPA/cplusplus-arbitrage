#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <functional>
#include <optional>
#include <mutex>
#include <cmath>

namespace arb {

// ============================================================================
// Multi-Exchange Arbitrage Scanner
//
// Mathematical framework for detecting cross-exchange arbitrage opportunities.
// NOTE: Detection != Profitable execution. See docs/MULTI_EXCHANGE_ARBITRAGE.md
// ============================================================================

struct ExchangeQuote {
    std::string exchange;
    std::string symbol;         // e.g., "BTC/USDT"
    double bid_price{0};
    double bid_size{0};
    double ask_price{0};
    double ask_size{0};
    double taker_fee_bps{10};   // Default 0.10%
    double maker_fee_bps{10};
    uint64_t timestamp_us{0};
    bool is_valid{false};

    double spread() const { return ask_price - bid_price; }
    double spread_bps() const { return (spread() / mid_price()) * 10000; }
    double mid_price() const { return (bid_price + ask_price) / 2.0; }

    // Quote age in microseconds
    uint64_t age_us() const {
        auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        return now - timestamp_us;
    }

    bool is_stale(uint64_t max_age_us = 1000000) const {  // Default 1 second
        return age_us() > max_age_us;
    }
};

struct CrossExchangeOpportunity {
    // Where to execute
    std::string buy_exchange;
    std::string sell_exchange;
    std::string symbol;

    // Prices
    double buy_price;           // Best ask on buy exchange (we pay this)
    double sell_price;          // Best bid on sell exchange (we receive this)

    // Spreads
    double gross_spread_bps;    // Before fees
    double net_spread_bps;      // After fees (this is what matters)

    // Size constraints
    double buy_size_available;
    double sell_size_available;
    double max_executable_size;

    // Expected profit
    double expected_profit_usd;
    double profit_per_unit;

    // Timing
    uint64_t detected_at_us;
    uint64_t buy_quote_age_us;
    uint64_t sell_quote_age_us;

    // Quality metrics
    double confidence;          // 0-1, based on quote freshness and size
    bool is_actionable;         // Passes all filters

    std::string reason;         // Why actionable or not
};

struct TriangularOpportunity {
    std::string exchange;

    // The three legs: A → B → C → A
    std::string pair_1;         // e.g., "BTC/USDT" (sell BTC for USDT)
    std::string pair_2;         // e.g., "ETH/USDT" (buy ETH with USDT)
    std::string pair_3;         // e.g., "BTC/ETH" (buy BTC with ETH)

    double rate_1;
    double rate_2;
    double rate_3;

    // Cycle result: start with 1 unit of A, end with X units of A
    double cycle_return;        // > 1.0 means profit
    double gross_profit_bps;    // (cycle_return - 1) * 10000
    double net_profit_bps;      // After 3x fees

    double max_size;            // Limited by smallest leg
    double expected_profit_usd;

    uint64_t detected_at_us;
    bool is_actionable;
    std::string reason;
};

struct FundingRateOpportunity {
    std::string exchange;
    std::string symbol;

    double spot_price;
    double perp_price;
    double funding_rate;        // Per period (usually 8h)
    double annualized_rate;

    // Strategy: Long spot + Short perp (when funding positive)
    //           Short spot + Long perp (when funding negative)
    std::string direction;      // "long_basis" or "short_basis"

    double expected_return_8h;
    double expected_return_daily;
    double expected_return_annual;

    // Risk metrics
    double basis_risk;          // Spot-perp spread volatility
    double liquidation_price;   // If using leverage

    bool is_actionable;
    std::string reason;
};

// Callback types
using OpportunityCallback = std::function<void(const CrossExchangeOpportunity&)>;
using TriangularCallback = std::function<void(const TriangularOpportunity&)>;
using FundingCallback = std::function<void(const FundingRateOpportunity&)>;

// Scanner configuration (defined outside class to allow default parameter)
struct ScannerConfig {
    double min_net_spread_bps = 5.0;       // Minimum 0.05% after fees
    double min_profit_usd = 1.0;            // Minimum $1 expected profit
    double max_quote_age_us = 500000;       // 500ms max staleness
    double min_size_usd = 100;              // Minimum position size
    double max_size_usd = 10000;            // Maximum position size
    bool require_both_fresh = true;         // Both quotes must be fresh
};

class MultiExchangeScanner {
public:
    using Config = ScannerConfig;

    explicit MultiExchangeScanner(const Config& config = Config());

    // Update quotes from exchanges
    void update_quote(const ExchangeQuote& quote);

    // Scan for opportunities
    std::vector<CrossExchangeOpportunity> scan_cross_exchange();
    std::vector<TriangularOpportunity> scan_triangular(const std::string& exchange);
    std::vector<FundingRateOpportunity> scan_funding_rates();

    // Set callbacks for real-time alerts
    void set_opportunity_callback(OpportunityCallback cb) { on_opportunity_ = std::move(cb); }
    void set_triangular_callback(TriangularCallback cb) { on_triangular_ = std::move(cb); }
    void set_funding_callback(FundingCallback cb) { on_funding_ = std::move(cb); }

    // Statistics
    struct Stats {
        uint64_t quotes_processed{0};
        uint64_t opportunities_detected{0};
        uint64_t actionable_opportunities{0};
        double best_spread_seen_bps{0};
        double total_theoretical_profit{0};
        uint64_t scan_count{0};
    };

    Stats stats() const { return stats_; }

    // Exchange fee configuration
    void set_exchange_fees(const std::string& exchange, double maker_bps, double taker_bps);

private:
    Config config_;
    Stats stats_;

    // Quote storage: exchange -> symbol -> quote
    std::map<std::string, std::map<std::string, ExchangeQuote>> quotes_;

    // Fee storage: exchange -> {maker_bps, taker_bps}
    std::map<std::string, std::pair<double, double>> exchange_fees_;

    // Callbacks
    OpportunityCallback on_opportunity_;
    TriangularCallback on_triangular_;
    FundingCallback on_funding_;

    mutable std::mutex mutex_;

    // Helper functions
    double get_taker_fee_bps(const std::string& exchange) const;
    double get_maker_fee_bps(const std::string& exchange) const;

    CrossExchangeOpportunity evaluate_pair(
        const ExchangeQuote& buy_quote,
        const ExchangeQuote& sell_quote
    );

    TriangularOpportunity evaluate_triangle(
        const std::string& exchange,
        const ExchangeQuote& q1,
        const ExchangeQuote& q2,
        const ExchangeQuote& q3
    );
};

// ============================================================================
// Mathematical Utilities for Arbitrage Detection
// ============================================================================

namespace math {

// Calculate net spread after fees (in basis points)
inline double net_spread_bps(double buy_price, double sell_price,
                             double buy_fee_bps, double sell_fee_bps) {
    double gross_spread = (sell_price - buy_price) / buy_price * 10000;
    return gross_spread - buy_fee_bps - sell_fee_bps;
}

// Calculate triangular arbitrage cycle return
// Returns > 1.0 if profitable before fees
inline double triangular_cycle(double rate_ab, double rate_bc, double rate_ca) {
    // Start with 1 unit of A
    // A → B: get rate_ab units of B
    // B → C: get rate_ab * rate_bc units of C
    // C → A: get rate_ab * rate_bc * rate_ca units of A
    return rate_ab * rate_bc * rate_ca;
}

// Calculate cointegration spread z-score for stat arb
inline double zscore(double current_spread, double mean_spread, double std_spread) {
    if (std_spread <= 0) return 0;
    return (current_spread - mean_spread) / std_spread;
}

// Annualize a periodic return
inline double annualize(double periodic_return, int periods_per_year) {
    return std::pow(1.0 + periodic_return, periods_per_year) - 1.0;
}

// Calculate required spread to break even given fees
inline double breakeven_spread_bps(double buy_fee_bps, double sell_fee_bps,
                                   double slippage_bps = 0) {
    return buy_fee_bps + sell_fee_bps + slippage_bps;
}

// Estimate execution probability based on size and book depth
inline double fill_probability(double order_size, double available_size) {
    if (available_size <= 0) return 0;
    if (order_size <= available_size) return 0.95;  // High prob for small orders
    return 0.95 * (available_size / order_size);    // Linear decrease
}

// Kelly criterion for optimal position sizing
// edge = expected return, odds = win/loss ratio
inline double kelly_fraction(double edge, double win_prob) {
    if (win_prob <= 0 || win_prob >= 1) return 0;
    double lose_prob = 1.0 - win_prob;
    // f* = (p * b - q) / b where b = win/loss ratio (assumed 1:1 for arb)
    // Simplified for arbitrage: f* = edge (since near-certain)
    return std::min(0.25, edge);  // Cap at 25% of capital
}

}  // namespace math

}  // namespace arb
