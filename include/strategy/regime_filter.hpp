#pragma once

#include <deque>
#include <vector>
#include <string>
#include <map>
#include <atomic>
#include <mutex>
#include "common/types.hpp"
#include "market_data/order_book.hpp"

namespace arb {

/**
 * Market regime classification.
 */
enum class MarketRegime {
    FAVORABLE,     // Good conditions for trading
    NEUTRAL,       // Normal conditions
    UNFAVORABLE,   // Bad conditions, reduce activity
    DANGEROUS      // Very bad, avoid trading
};

inline std::string regime_to_string(MarketRegime r) {
    switch (r) {
        case MarketRegime::FAVORABLE: return "FAVORABLE";
        case MarketRegime::NEUTRAL: return "NEUTRAL";
        case MarketRegime::UNFAVORABLE: return "UNFAVORABLE";
        case MarketRegime::DANGEROUS: return "DANGEROUS";
    }
    return "UNKNOWN";
}

/**
 * Regime assessment breakdown.
 */
struct RegimeAssessment {
    MarketRegime overall_regime{MarketRegime::NEUTRAL};

    // Component scores (0 = bad, 1 = good)
    double volatility_score{0.5};
    double liquidity_score{0.5};
    double spread_score{0.5};
    double time_score{0.5};        // Time to expiry
    double momentum_score{0.5};    // Recent price momentum

    // Raw metrics
    double current_volatility{0.0};
    double avg_spread_bps{0.0};
    double total_liquidity_usd{0.0};
    double hours_to_expiry{0.0};
    double price_momentum{0.0};    // Recent direction

    // Recommendations
    double size_multiplier{1.0};   // Adjust position size
    double edge_multiplier{1.0};   // Require more edge

    std::vector<std::string> warnings;
    std::string summary() const;
};

/**
 * Market regime filter determines when trading conditions are suitable.
 *
 * DESIGN:
 * - Analyzes multiple factors to classify current market conditions
 * - Provides size and edge adjustments based on regime
 * - Tracks conditions over time for trend detection
 * - Warns about deteriorating conditions
 *
 * FACTORS:
 * 1. Volatility - High vol = uncertain, reduce size
 * 2. Liquidity - Low liquidity = slippage risk
 * 3. Spreads - Wide spreads = high execution cost
 * 4. Time to expiry - Near expiry = increased uncertainty
 * 5. Momentum - Strong trends may indicate news/events
 */
class RegimeFilter {
public:
    struct Config {
        // Volatility thresholds (hourly)
        double favorable_vol{0.005};     // <0.5% hourly = favorable
        double neutral_vol{0.01};        // <1% = neutral
        double unfavorable_vol{0.02};    // <2% = unfavorable
        // >2% = dangerous

        // Spread thresholds (bps)
        double favorable_spread{20.0};   // <20bps = favorable
        double neutral_spread{50.0};     // <50bps = neutral
        double unfavorable_spread{100.0};// <100bps = unfavorable

        // Liquidity thresholds (USD per side)
        double favorable_liquidity{100.0};
        double neutral_liquidity{50.0};
        double unfavorable_liquidity{20.0};

        // Time to expiry thresholds (hours)
        double favorable_hours{4.0};     // >4h = favorable
        double neutral_hours{1.0};       // >1h = neutral
        double unfavorable_hours{0.25};  // >15min = unfavorable

        // Momentum thresholds (magnitude of recent move)
        double favorable_momentum{0.005};
        double unfavorable_momentum{0.02};

        // Weighting for overall score
        double vol_weight{0.30};
        double liquidity_weight{0.25};
        double spread_weight{0.20};
        double time_weight{0.15};
        double momentum_weight{0.10};

        // Size/edge adjustments by regime
        struct RegimeAdjustment {
            double size_mult{1.0};
            double edge_mult{1.0};
        };
        std::map<MarketRegime, RegimeAdjustment> adjustments = {
            {MarketRegime::FAVORABLE, {1.5, 0.8}},
            {MarketRegime::NEUTRAL, {1.0, 1.0}},
            {MarketRegime::UNFAVORABLE, {0.5, 1.5}},
            {MarketRegime::DANGEROUS, {0.0, 999.0}}  // Don't trade
        };
    };

    explicit RegimeFilter(const Config& config = Config{});

    // Update inputs
    void update_btc_price(double price, Timestamp time);
    void update_market_data(const BinaryMarketBook& book);

    // Core assessment
    RegimeAssessment assess(const BinaryMarketBook& book, Timestamp now) const;
    RegimeAssessment assess_general() const;  // Without specific market

    // Quick queries
    MarketRegime current_regime() const { return current_regime_.load(); }
    bool should_trade() const;
    double recommended_size_multiplier() const;
    double recommended_edge_multiplier() const;

    // Apply filter to signal
    struct FilterResult {
        bool should_trade{true};
        double adjusted_size{0.0};
        double adjusted_min_edge{0.0};
        std::string reason;
    };

    FilterResult apply(double base_size, double min_edge, const BinaryMarketBook& book) const;

    // History
    struct RegimeHistory {
        WallClock timestamp;
        MarketRegime regime;
        double overall_score;
    };
    std::vector<RegimeHistory> get_history(size_t max_entries = 100) const;

    // Stats
    std::string status_summary() const;

private:
    Config config_;

    // BTC price history
    struct PricePoint {
        double price;
        Timestamp time;
    };
    mutable std::mutex price_mutex_;
    std::deque<PricePoint> price_history_;
    static constexpr size_t MAX_PRICE_HISTORY = 1000;

    // Market book cache
    mutable std::mutex book_mutex_;
    std::map<std::string, BinaryMarketBook> book_cache_;

    // Current regime
    std::atomic<MarketRegime> current_regime_{MarketRegime::NEUTRAL};
    mutable std::mutex history_mutex_;
    std::deque<RegimeHistory> regime_history_;

    // Score calculation
    double calculate_volatility_score() const;
    double calculate_liquidity_score(const BinaryMarketBook& book) const;
    double calculate_spread_score(const BinaryMarketBook& book) const;
    double calculate_time_score(const BinaryMarketBook& book) const;
    double calculate_momentum_score() const;

    double score_to_regime_score(double raw, double favorable, double neutral, double unfavorable) const;
    MarketRegime score_to_regime(double overall_score) const;

    // Volatility calculation
    double calculate_hourly_volatility() const;
    double calculate_recent_momentum() const;
};

} // namespace arb
