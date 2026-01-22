#pragma once

#include <deque>
#include <vector>
#include <cmath>
#include "strategy/strategy_base.hpp"
#include "common/types.hpp"
#include "market_data/order_book.hpp"

namespace arb {

/**
 * Volatility-adjusted fair value strategy (S5).
 *
 * DESIGN:
 * - Uses Black-Scholes-inspired probability model
 * - Adjusts fair value based on realized BTC volatility
 * - Compares fair probability to market price
 * - Only trades when divergence exceeds threshold
 *
 * KEY INSIGHT:
 * Binary outcome markets have implied volatility embedded in their prices.
 * If realized volatility differs from implied, there's potential edge.
 *
 * For a "BTC above $X at time T" market:
 * - Fair probability = N(d2) where d2 = (ln(S/K) + (r - 0.5*sigma^2)*T) / (sigma*sqrt(T))
 * - Compare to market probability = 1 - YES_ask
 */
class VolatilityFairValueStrategy : public StrategyBase {
public:
    struct Config {
        // Volatility estimation
        int vol_lookback_periods{24};           // Hours of data for vol estimation
        int vol_sample_interval_seconds{300};   // 5-minute samples
        double default_annualized_vol{0.50};    // 50% annual vol if no data

        // Trading thresholds
        double min_probability_edge{0.03};      // 3% probability edge required
        double min_cents_edge{1.5};             // 1.5 cents minimum edge
        double max_probability{0.95};           // Don't trade if prob > 95%
        double min_probability{0.05};           // Don't trade if prob < 5%

        // Market structure
        double max_spread_percent{0.05};        // Max 5% spread to trade
        double min_liquidity_usd{10.0};         // Min $10 on each side

        // Position sizing
        double kelly_fraction{0.25};            // Use 25% Kelly
        double max_edge_to_size_ratio{0.10};    // Edge as % of position
    };

    explicit VolatilityFairValueStrategy(const StrategyConfig& base_config, const Config& config = Config{});

    std::vector<Signal> evaluate(
        const BinaryMarketBook& book,
        const BtcPrice& btc_price,
        Timestamp now
    ) override;

    // Volatility estimation
    void update_btc_price(double price, Timestamp time);
    double current_volatility() const;
    double annualized_volatility() const;

    // Fair value calculation
    struct FairValueResult {
        double fair_probability{0.5};
        double market_probability{0.5};
        double probability_edge{0.0};
        double implied_vol{0.0};
        double realized_vol{0.0};
        bool is_tradeable{false};
        std::string reason;
    };

    FairValueResult calculate_fair_value(
        const BinaryMarketBook& book,
        double btc_current,
        double btc_strike,
        double time_to_expiry_years
    ) const;

    // Kelly sizing
    double kelly_size(double edge, double probability, double max_size) const;

private:
    Config vol_config_;

    // BTC price history for volatility
    struct PricePoint {
        double price;
        Timestamp time;
    };
    std::deque<PricePoint> price_history_;
    mutable double cached_vol_{0.0};
    mutable Timestamp vol_cache_time_;

    // Statistical functions
    double calculate_realized_vol() const;
    double calculate_log_returns_vol(const std::vector<double>& returns) const;
    double normal_cdf(double x) const;
    double normal_pdf(double x) const;

    // Black-Scholes
    double bs_d1(double S, double K, double T, double r, double sigma) const;
    double bs_d2(double d1, double sigma, double T) const;
    double bs_probability(double S, double K, double T, double r, double sigma) const;

    // Market probability extraction
    double extract_market_probability(const BinaryMarketBook& book) const;
    double implied_volatility_from_price(double market_prob, double S, double K, double T) const;

    // Helpers
    double parse_strike_from_market(const std::string& market_id, const std::string& question) const;
    double parse_time_to_expiry(const WallClock& end_date) const;
    bool is_market_tradeable(const BinaryMarketBook& book) const;
};

} // namespace arb
