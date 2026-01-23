#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <cmath>

namespace arb {

// ============================================================================
// Funding Rate Arbitrage
//
// This strategy has a REALISTIC chance of working because:
// 1. Operates on 8-hour cycles (not milliseconds)
// 2. Less competition from HFT (holding period too long for them)
// 3. Mathematically guaranteed returns if funded properly
//
// Strategy:
// - When funding rate is POSITIVE (longs pay shorts):
//   → Short perpetual + Long spot = Collect funding
//
// - When funding rate is NEGATIVE (shorts pay longs):
//   → Long perpetual + Short spot = Collect funding
// ============================================================================

struct FundingRateData {
    std::string exchange;
    std::string symbol;

    double spot_price{0};
    double perp_price{0};
    double mark_price{0};

    double funding_rate{0};         // Current period rate (e.g., 0.0001 = 0.01%)
    double predicted_rate{0};       // Next period predicted rate
    int64_t next_funding_time{0};   // Unix timestamp

    // Annualized returns
    double annualized_rate() const {
        // 3 funding periods per day * 365 days
        return funding_rate * 3 * 365 * 100;  // As percentage
    }

    // Basis (perp premium/discount to spot)
    double basis() const {
        return (perp_price - spot_price) / spot_price;
    }

    double basis_bps() const {
        return basis() * 10000;
    }
};

struct FundingArbPosition {
    std::string exchange;
    std::string symbol;

    // Position details
    double spot_size{0};            // Positive = long spot
    double perp_size{0};            // Negative = short perp
    double entry_spot_price{0};
    double entry_perp_price{0};
    double entry_funding_rate{0};

    // P&L tracking
    double total_funding_collected{0};
    double spot_pnl{0};
    double perp_pnl{0};
    double fees_paid{0};

    int funding_periods_held{0};
    int64_t entry_time{0};

    // Current values
    double current_spot_price{0};
    double current_perp_price{0};

    // Calculations
    double net_pnl() const {
        return total_funding_collected + spot_pnl + perp_pnl - fees_paid;
    }

    double annualized_return() const {
        if (funding_periods_held == 0) return 0;
        double periods_per_year = 3 * 365;  // 3 per day
        double per_period_return = net_pnl() / (spot_size * entry_spot_price);
        return per_period_return * (periods_per_year / funding_periods_held) * 100;
    }

    // Risk metrics
    double basis_risk() const {
        // How much the basis has moved against us
        double entry_basis = (entry_perp_price - entry_spot_price) / entry_spot_price;
        double current_basis = (current_perp_price - current_spot_price) / current_spot_price;
        return (current_basis - entry_basis) * 10000;  // In bps
    }
};

class FundingArbitrageStrategy {
public:
    struct Config {
        double min_annualized_return{10.0};   // Minimum 10% annualized
        double max_basis_bps{50.0};           // Don't enter if basis > 0.5%
        double min_position_usd{1000};
        double max_position_usd{50000};
        double stop_loss_bps{100};            // Exit if basis moves 1% against
        bool allow_leverage{false};           // Perp margin
        double max_leverage{1.0};             // 1x = delta neutral
    };

    explicit FundingArbitrageStrategy(const Config& config = Config{});

    // Evaluate opportunity
    struct Evaluation {
        bool should_enter{false};
        std::string direction;          // "long_basis" or "short_basis"
        double expected_return_8h;
        double expected_return_annual;
        double recommended_size_usd;
        double risk_score;              // 0-1, lower is better
        std::string reason;
    };

    Evaluation evaluate(const FundingRateData& data);

    // Entry/exit signals
    bool should_enter(const FundingRateData& data, Evaluation& eval);
    bool should_exit(const FundingArbPosition& position, const FundingRateData& current);

    // Position sizing (Kelly-inspired)
    double calculate_position_size(const FundingRateData& data, double capital);

    // Risk-adjusted return
    double sharpe_estimate(const FundingRateData& data, double volatility_annual = 0.5);

private:
    Config config_;

    // Historical data for better estimates
    std::map<std::string, std::vector<double>> historical_rates_;

    double estimate_rate_persistence(const std::string& symbol);
};

// ============================================================================
// Implementation
// ============================================================================

inline FundingArbitrageStrategy::FundingArbitrageStrategy(const Config& config)
    : config_(config) {}

inline FundingArbitrageStrategy::Evaluation
FundingArbitrageStrategy::evaluate(const FundingRateData& data) {
    Evaluation eval;

    double annual_return = data.annualized_rate();
    eval.expected_return_8h = data.funding_rate * 100;  // As percentage
    eval.expected_return_annual = annual_return;

    // Determine direction
    if (data.funding_rate > 0) {
        // Longs pay shorts → We want to be short perp, long spot
        eval.direction = "long_basis";  // Long spot, short perp
    } else {
        // Shorts pay longs → We want to be long perp, short spot
        eval.direction = "short_basis";
    }

    // Check if meets threshold
    if (std::abs(annual_return) < config_.min_annualized_return) {
        eval.should_enter = false;
        eval.reason = fmt::format("Annual return {:.1f}% < {:.1f}% threshold",
                                  std::abs(annual_return), config_.min_annualized_return);
        return eval;
    }

    // Check basis risk
    if (std::abs(data.basis_bps()) > config_.max_basis_bps) {
        eval.should_enter = false;
        eval.reason = fmt::format("Basis {:.1f}bps > {:.1f}bps max",
                                  std::abs(data.basis_bps()), config_.max_basis_bps);
        return eval;
    }

    // Calculate risk score (lower is better)
    // Higher basis = higher risk of convergence loss
    // Higher rate = higher reward
    double reward = std::abs(annual_return) / 100;
    double risk = std::abs(data.basis_bps()) / 100;
    eval.risk_score = risk / (reward + 0.01);  // Avoid div by zero

    // Position sizing based on Kelly-like criterion
    // More conservative than pure Kelly
    double kelly_fraction = reward / (reward + risk + 0.1);
    eval.recommended_size_usd = config_.max_position_usd * kelly_fraction * 0.5;  // Half-Kelly
    eval.recommended_size_usd = std::max(config_.min_position_usd, eval.recommended_size_usd);
    eval.recommended_size_usd = std::min(config_.max_position_usd, eval.recommended_size_usd);

    eval.should_enter = true;
    eval.reason = fmt::format("{}: {:.1f}% annual, basis={:.1f}bps, size=${:.0f}",
                              eval.direction, annual_return, data.basis_bps(),
                              eval.recommended_size_usd);

    return eval;
}

inline bool FundingArbitrageStrategy::should_enter(
    const FundingRateData& data, Evaluation& eval
) {
    eval = evaluate(data);
    return eval.should_enter;
}

inline bool FundingArbitrageStrategy::should_exit(
    const FundingArbPosition& position,
    const FundingRateData& current
) {
    // Exit conditions:

    // 1. Funding rate flipped sign (strategy no longer works)
    if ((position.entry_funding_rate > 0 && current.funding_rate < 0) ||
        (position.entry_funding_rate < 0 && current.funding_rate > 0)) {
        return true;
    }

    // 2. Basis moved too much against us (stop loss)
    if (std::abs(position.basis_risk()) > config_.stop_loss_bps) {
        return true;
    }

    // 3. Return dropped below threshold
    if (std::abs(current.annualized_rate()) < config_.min_annualized_return * 0.5) {
        return true;
    }

    return false;
}

inline double FundingArbitrageStrategy::calculate_position_size(
    const FundingRateData& data, double capital
) {
    auto eval = evaluate(data);
    if (!eval.should_enter) return 0;

    // Don't risk more than 20% of capital on any single position
    double max_from_capital = capital * 0.20;

    return std::min({eval.recommended_size_usd, max_from_capital, config_.max_position_usd});
}

inline double FundingArbitrageStrategy::sharpe_estimate(
    const FundingRateData& data, double volatility_annual
) {
    // Sharpe = (Return - Risk_free) / Volatility
    // Assume risk-free = 5%
    double annual_return = std::abs(data.annualized_rate()) / 100;
    double risk_free = 0.05;

    if (volatility_annual <= 0) return 0;
    return (annual_return - risk_free) / volatility_annual;
}

}  // namespace arb
