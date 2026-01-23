#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <optional>
#include <functional>
#include <fmt/format.h>

namespace arb {

// ============================================================================
// CROSS-EXCHANGE FUNDING RATE DISPERSION: FULL IMPLEMENTATION
//
// This is not pair trading. This is GRAPH OPTIMIZATION over constrained flows.
//
// KEY INSIGHT: Funding dispersion exists because markets are STRUCTURALLY
// FORCED to disagree. Trader populations differ. Margin rules differ.
// Capital cannot teleport. These constraints create persistent rent.
//
// You are harvesting structural rent, not inefficiency.
// ============================================================================

// ============================================================================
// PART 1: FUNDING RATE TIME SERIES MODEL
//
// Funding is a stochastic process with:
// - Mean reversion (extreme funding pulls back)
// - Clustering (high funding tends to stay high)
// - Regime shifts (sudden changes in market sentiment)
//
// We model this to predict PERSISTENCE, not just current value.
// ============================================================================

struct FundingObservation {
    std::string exchange;
    std::string symbol;
    double rate{0};             // Raw funding rate (e.g., 0.0003 = 0.03%)
    int64_t timestamp{0};       // Unix timestamp
    int64_t period_seconds{0};  // Funding period (usually 28800 = 8h)
};

class FundingTimeSeries {
public:
    static constexpr size_t MAX_HISTORY = 500;  // ~166 days of 8h data

    void add_observation(const FundingObservation& obs) {
        auto& history = data_[obs.exchange];
        history.push_back(obs);
        if (history.size() > MAX_HISTORY) {
            history.pop_front();
        }
    }

    // Current funding rate for an exchange
    std::optional<double> current_rate(const std::string& exchange) const {
        auto it = data_.find(exchange);
        if (it == data_.end() || it->second.empty()) return std::nullopt;
        return it->second.back().rate;
    }

    // Rolling mean of funding rate
    double rolling_mean(const std::string& exchange, size_t window = 21) const {
        auto it = data_.find(exchange);
        if (it == data_.end() || it->second.empty()) return 0;

        const auto& history = it->second;
        size_t n = std::min(window, history.size());
        double sum = 0;
        for (size_t i = history.size() - n; i < history.size(); i++) {
            sum += history[i].rate;
        }
        return sum / n;
    }

    // Rolling volatility of funding rate
    double rolling_volatility(const std::string& exchange, size_t window = 21) const {
        auto it = data_.find(exchange);
        if (it == data_.end() || it->second.size() < 2) return 0.0001;

        const auto& history = it->second;
        size_t n = std::min(window, history.size());
        double mean = rolling_mean(exchange, window);

        double sq_sum = 0;
        for (size_t i = history.size() - n; i < history.size(); i++) {
            double diff = history[i].rate - mean;
            sq_sum += diff * diff;
        }
        return std::sqrt(sq_sum / (n - 1));
    }

    // Autocorrelation at lag k (measures persistence)
    // High autocorrelation = funding tends to stay elevated
    double autocorrelation(const std::string& exchange, size_t lag = 1) const {
        auto it = data_.find(exchange);
        if (it == data_.end() || it->second.size() < lag + 10) return 0;

        const auto& history = it->second;
        double mean = rolling_mean(exchange, history.size());

        double numerator = 0, denominator = 0;
        for (size_t i = lag; i < history.size(); i++) {
            double x_t = history[i].rate - mean;
            double x_t_k = history[i - lag].rate - mean;
            numerator += x_t * x_t_k;
            denominator += x_t * x_t;
        }

        if (denominator == 0) return 0;
        return numerator / denominator;
    }

    // Estimate funding persistence: probability that sign persists for N more periods
    // Uses empirical transition probabilities
    double persistence_probability(const std::string& exchange, size_t horizon = 3) const {
        auto it = data_.find(exchange);
        if (it == data_.end() || it->second.size() < 50) return 0.5;

        const auto& history = it->second;

        // Count transitions
        int same_sign_count = 0;
        int total_transitions = 0;

        for (size_t i = horizon; i < history.size(); i++) {
            bool current_positive = history[i].rate > 0;
            bool past_positive = history[i - horizon].rate > 0;
            if (current_positive == past_positive) {
                same_sign_count++;
            }
            total_transitions++;
        }

        if (total_transitions == 0) return 0.5;
        return static_cast<double>(same_sign_count) / total_transitions;
    }

    // Z-score: how extreme is current funding relative to history?
    double zscore(const std::string& exchange, size_t window = 50) const {
        auto it = data_.find(exchange);
        if (it == data_.end() || it->second.empty()) return 0;

        double current = it->second.back().rate;
        double mean = rolling_mean(exchange, window);
        double vol = rolling_volatility(exchange, window);

        if (vol < 1e-10) return 0;
        return (current - mean) / vol;
    }

    // Get all exchanges with data
    std::vector<std::string> exchanges() const {
        std::vector<std::string> result;
        for (const auto& [ex, _] : data_) {
            result.push_back(ex);
        }
        return result;
    }

private:
    std::map<std::string, std::deque<FundingObservation>> data_;
};

// ============================================================================
// PART 2: MULTI-EXCHANGE GRAPH OPTIMIZATION
//
// You don't just trade pairs. You optimize over the FULL GRAPH of exchanges.
//
// Given N exchanges, find the allocation that maximizes:
//   Expected funding capture - Risk penalty
//
// Subject to:
//   Net delta = 0 (no directional exposure)
//   Position per exchange <= limit
//   Total margin used <= budget
// ============================================================================

struct ExchangeNode {
    std::string name;
    double funding_rate{0};
    double funding_zscore{0};
    double persistence_prob{0};
    double max_position{0};         // Position limit on this exchange
    double margin_requirement{0};   // Margin per unit of position
    double taker_fee_bps{0};
    double maker_fee_bps{0};

    // Estimated risk metrics
    double basis_volatility{0};     // How much perp-spot basis moves
    double exchange_risk{0};        // Probability of exchange failure (annual)
};

struct GraphAllocation {
    // position[exchange] > 0 means LONG perp (we pay funding if positive)
    // position[exchange] < 0 means SHORT perp (we receive funding if positive)
    std::map<std::string, double> positions;

    double expected_funding_per_period{0};  // Net funding we expect to receive
    double expected_annual_return{0};
    double total_risk{0};
    double sharpe_estimate{0};

    bool is_valid{false};
    std::string reason;
};

// Config struct defined outside class for default parameter compatibility
struct FundingGraphOptimizerConfig {
    double max_position_per_exchange = 0.25;  // Fraction of capital
    double max_total_leverage = 3.0;          // Total leverage across all exchanges
    double risk_aversion = 1.0;               // Higher = more conservative
    double min_spread_threshold = 0.0001;     // 0.01% minimum
    double exchange_risk_weight = 0.1;        // How much to penalize exchange risk
};

class FundingGraphOptimizer {
public:
    using Config = FundingGraphOptimizerConfig;

    explicit FundingGraphOptimizer(const Config& config = Config())
        : config_(config) {}

    // Update exchange data
    void update_exchange(const ExchangeNode& node) {
        exchanges_[node.name] = node;
    }

    // Core optimization: find best allocation across all exchanges
    GraphAllocation optimize(double total_capital) {
        GraphAllocation result;

        if (exchanges_.size() < 2) {
            result.is_valid = false;
            result.reason = "Need at least 2 exchanges";
            return result;
        }

        // Sort exchanges by funding rate (highest to lowest)
        std::vector<std::pair<std::string, double>> sorted_exchanges;
        for (const auto& [name, node] : exchanges_) {
            sorted_exchanges.emplace_back(name, node.funding_rate);
        }
        std::sort(sorted_exchanges.begin(), sorted_exchanges.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        // Calculate spread
        double max_funding = sorted_exchanges.front().second;
        double min_funding = sorted_exchanges.back().second;
        double spread = max_funding - min_funding;

        if (spread < config_.min_spread_threshold) {
            result.is_valid = false;
            result.reason = fmt::format("Spread {:.4f}% below threshold {:.4f}%",
                                        spread * 100, config_.min_spread_threshold * 100);
            return result;
        }

        // GREEDY ALLOCATION (can be replaced with convex optimization)
        //
        // Strategy: Short the highest funding exchanges, long the lowest
        // Maintain delta neutrality

        double max_per_exchange = total_capital * config_.max_position_per_exchange;
        double total_short = 0;
        double total_long = 0;
        double total_funding = 0;
        double total_risk_sq = 0;

        // Short top exchanges (receive funding when positive)
        for (size_t i = 0; i < sorted_exchanges.size() / 2; i++) {
            const auto& [name, funding] = sorted_exchanges[i];
            const auto& node = exchanges_[name];

            double position = -std::min(max_per_exchange, node.max_position);
            result.positions[name] = position;
            total_short += std::abs(position);

            // We receive funding when short and funding is positive
            total_funding += std::abs(position) * funding;

            // Add risk contribution
            double risk = node.basis_volatility * std::abs(position);
            risk += node.exchange_risk * config_.exchange_risk_weight * std::abs(position);
            total_risk_sq += risk * risk;
        }

        // Long bottom exchanges (pay funding, but it's lower or negative)
        for (size_t i = sorted_exchanges.size() / 2; i < sorted_exchanges.size(); i++) {
            const auto& [name, funding] = sorted_exchanges[i];
            const auto& node = exchanges_[name];

            double position = std::min(max_per_exchange, node.max_position);
            result.positions[name] = position;
            total_long += position;

            // We pay funding when long and funding is positive
            total_funding -= position * funding;

            double risk = node.basis_volatility * position;
            risk += node.exchange_risk * config_.exchange_risk_weight * position;
            total_risk_sq += risk * risk;
        }

        // Ensure delta neutrality by scaling
        if (total_long != total_short && total_long > 0 && total_short > 0) {
            double scale = total_short / total_long;
            for (auto& [name, pos] : result.positions) {
                if (pos > 0) pos *= scale;
            }
        }

        result.expected_funding_per_period = total_funding;
        result.expected_annual_return = total_funding * 3 * 365 / total_capital * 100;
        result.total_risk = std::sqrt(total_risk_sq);
        result.sharpe_estimate = (total_funding * 3 * 365) /
                                 (result.total_risk * std::sqrt(365.0) + 1e-10);

        result.is_valid = true;
        result.reason = fmt::format(
            "Spread: {:.3f}%, Expected annual: {:.1f}%, Sharpe: {:.1f}",
            spread * 100, result.expected_annual_return, result.sharpe_estimate
        );

        return result;
    }

    // Advanced: rank exchanges by risk-adjusted funding
    std::vector<std::pair<std::string, double>> rank_exchanges() const {
        std::vector<std::pair<std::string, double>> result;

        for (const auto& [name, node] : exchanges_) {
            // Risk-adjusted funding = funding / (basis_vol + exchange_risk)
            double risk = node.basis_volatility + node.exchange_risk * config_.exchange_risk_weight;
            double score = node.funding_rate / (risk + 0.0001);
            result.emplace_back(name, score);
        }

        std::sort(result.begin(), result.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        return result;
    }

private:
    Config config_;
    std::map<std::string, ExchangeNode> exchanges_;
};

// ============================================================================
// PART 3: FAILURE MODES AND KILL CONDITIONS
//
// Every strategy has ways to die. You must enumerate them.
// ============================================================================

enum class FailureMode {
    BASIS_DIVERGENCE,       // Perp prices diverge across exchanges
    FUNDING_FLIP,           // Funding reverses sign
    EXCHANGE_HALT,          // Withdrawals/trading halted
    LIQUIDATION_CASCADE,    // Extreme price move forces liquidation
    SPREAD_COLLAPSE,        // Everyone piles in, spread vanishes
    MARGIN_CALL,            // Insufficient margin due to MTM loss
    API_FAILURE,            // Can't execute exit
    REGULATORY_FREEZE,      // Account/funds frozen
};

struct FailureAnalysis {
    FailureMode mode;
    double probability;         // Estimated annual probability
    double expected_loss;       // As fraction of position
    double max_loss;           // Worst case loss
    std::string mitigation;
};

class FailureModeAnalyzer {
public:
    std::vector<FailureAnalysis> analyze_position(
        const GraphAllocation& allocation,
        const FundingTimeSeries& history
    ) {
        std::vector<FailureAnalysis> failures;

        // 1. BASIS DIVERGENCE
        // Perp prices on different exchanges can diverge, creating MTM loss
        {
            FailureAnalysis f;
            f.mode = FailureMode::BASIS_DIVERGENCE;
            f.probability = 0.3;  // Happens ~30% of years (minor)
            f.expected_loss = 0.02;  // 2% average when it happens
            f.max_loss = 0.10;  // 10% in extreme cases
            f.mitigation = "Set basis divergence stop at 0.5%, monitor hourly";
            failures.push_back(f);
        }

        // 2. FUNDING FLIP
        // Funding rates can suddenly reverse
        {
            FailureAnalysis f;
            f.mode = FailureMode::FUNDING_FLIP;
            f.probability = 0.5;  // Very common
            f.expected_loss = 0.005;  // Small: just exit
            f.max_loss = 0.02;  // If slow to react
            f.mitigation = "Monitor every funding period, exit when spread inverts";
            failures.push_back(f);
        }

        // 3. EXCHANGE HALT
        {
            FailureAnalysis f;
            f.mode = FailureMode::EXCHANGE_HALT;
            f.probability = 0.05;  // ~5% per year for any single exchange
            f.expected_loss = 0.25;  // 25% of position on that exchange
            f.max_loss = 1.0;  // Total loss of funds on that exchange
            f.mitigation = "Diversify across 3+ exchanges, max 25% per exchange";
            failures.push_back(f);
        }

        // 4. LIQUIDATION CASCADE
        {
            FailureAnalysis f;
            f.mode = FailureMode::LIQUIDATION_CASCADE;
            f.probability = 0.1;  // 10% chance per year of major event
            f.expected_loss = 0.05;  // 5% if hit
            f.max_loss = 0.30;  // Severe cascade
            f.mitigation = "Keep leverage < 3x, maintain 50% margin buffer";
            failures.push_back(f);
        }

        // 5. SPREAD COLLAPSE (crowding)
        {
            FailureAnalysis f;
            f.mode = FailureMode::SPREAD_COLLAPSE;
            f.probability = 0.2;  // Strategy becomes known
            f.expected_loss = 0.0;  // No loss, just reduced returns
            f.max_loss = 0.0;
            f.mitigation = "Accept reduced returns, don't oversize";
            failures.push_back(f);
        }

        return failures;
    }

    // Expected annual loss from all failure modes
    double expected_annual_loss(const std::vector<FailureAnalysis>& failures) {
        double total = 0;
        for (const auto& f : failures) {
            total += f.probability * f.expected_loss;
        }
        return total;
    }

    // Value at Risk (99th percentile annual loss)
    double var_99(const std::vector<FailureAnalysis>& failures) {
        // Simplified: assume worst cases are correlated
        double worst_total = 0;
        for (const auto& f : failures) {
            // Probability-weighted max loss
            worst_total += f.probability * f.max_loss;
        }
        return std::min(worst_total * 2.33, 1.0);  // Cap at 100%
    }
};

// ============================================================================
// PART 4: EXACT ENTRY/EXIT MATH
//
// Entry: When expected risk-adjusted return exceeds threshold
// Exit: When any kill condition triggers OR expected return drops below threshold
// ============================================================================

struct EntryDecision {
    bool should_enter{false};

    // Position specification
    GraphAllocation allocation;

    // Expected returns
    double expected_funding_per_period{0};
    double expected_annual_gross{0};     // Before risk adjustment
    double expected_annual_net{0};       // After expected losses
    double sharpe_ratio{0};

    // Confidence metrics
    double persistence_score{0};         // How likely funding persists
    double crowding_score{0};           // How crowded is this trade (0 = empty, 1 = crowded)

    std::string reason;
};

struct ExitDecision {
    bool should_exit{false};
    double urgency{0};                  // 0-1, higher = exit faster
    FailureMode trigger{FailureMode::SPREAD_COLLAPSE};
    std::string reason;
};

// Config struct defined outside class for default parameter compatibility
struct FundingArbDecisionEngineConfig {
    // Entry thresholds
    double min_expected_annual_return = 0.15;   // 15% minimum
    double min_sharpe = 2.0;                    // Minimum Sharpe ratio
    double min_persistence = 0.6;              // 60% chance funding persists

    // Exit thresholds
    double exit_spread_threshold = 0.00005;    // Exit if spread < 0.005%
    double max_basis_divergence = 0.005;       // 0.5%
    double max_drawdown = 0.03;                // 3%

    // Sizing
    double kelly_fraction = 0.5;               // Half-Kelly
    double max_position_size = 0.5;            // Max 50% of capital
};

class FundingArbDecisionEngine {
public:
    using Config = FundingArbDecisionEngineConfig;

    explicit FundingArbDecisionEngine(const Config& config = Config())
        : config_(config) {}

    // Master entry decision
    EntryDecision evaluate_entry(
        const FundingTimeSeries& history,
        const FundingGraphOptimizer& optimizer,
        double capital
    ) {
        EntryDecision decision;

        // Get optimal allocation
        decision.allocation = const_cast<FundingGraphOptimizer&>(optimizer).optimize(capital);

        if (!decision.allocation.is_valid) {
            decision.should_enter = false;
            decision.reason = decision.allocation.reason;
            return decision;
        }

        // Calculate expected returns
        decision.expected_funding_per_period = decision.allocation.expected_funding_per_period;
        decision.expected_annual_gross = decision.allocation.expected_annual_return;

        // Estimate losses from failure modes
        FailureModeAnalyzer failure_analyzer;
        auto failures = failure_analyzer.analyze_position(decision.allocation, history);
        double expected_loss = failure_analyzer.expected_annual_loss(failures);

        decision.expected_annual_net = decision.expected_annual_gross - expected_loss * 100;
        decision.sharpe_ratio = decision.allocation.sharpe_estimate;

        // Calculate persistence score (average across exchanges)
        double persistence_sum = 0;
        int count = 0;
        for (const auto& [exchange, pos] : decision.allocation.positions) {
            persistence_sum += history.persistence_probability(exchange, 3);
            count++;
        }
        decision.persistence_score = count > 0 ? persistence_sum / count : 0;

        // Check entry criteria
        std::vector<std::string> rejection_reasons;

        if (decision.expected_annual_net < config_.min_expected_annual_return * 100) {
            rejection_reasons.push_back(fmt::format(
                "Expected return {:.1f}% < {:.1f}% threshold",
                decision.expected_annual_net, config_.min_expected_annual_return * 100
            ));
        }

        if (decision.sharpe_ratio < config_.min_sharpe) {
            rejection_reasons.push_back(fmt::format(
                "Sharpe {:.1f} < {:.1f} threshold",
                decision.sharpe_ratio, config_.min_sharpe
            ));
        }

        if (decision.persistence_score < config_.min_persistence) {
            rejection_reasons.push_back(fmt::format(
                "Persistence {:.0f}% < {:.0f}% threshold",
                decision.persistence_score * 100, config_.min_persistence * 100
            ));
        }

        if (rejection_reasons.empty()) {
            decision.should_enter = true;
            decision.reason = fmt::format(
                "ENTER: Expected {:.1f}% net annual, Sharpe {:.1f}, Persistence {:.0f}%",
                decision.expected_annual_net, decision.sharpe_ratio, decision.persistence_score * 100
            );
        } else {
            decision.should_enter = false;
            decision.reason = fmt::format("{}", fmt::join(rejection_reasons, "; "));
        }

        return decision;
    }

    // Master exit decision
    ExitDecision evaluate_exit(
        const GraphAllocation& current_position,
        const FundingTimeSeries& history,
        double current_pnl_fraction  // Current P&L as fraction of position
    ) {
        ExitDecision decision;

        // KILL CONDITION 1: Spread collapsed
        double current_spread = 0;
        double max_funding = -1e9, min_funding = 1e9;
        for (const auto& [exchange, pos] : current_position.positions) {
            auto rate = history.current_rate(exchange);
            if (rate) {
                max_funding = std::max(max_funding, *rate);
                min_funding = std::min(min_funding, *rate);
            }
        }
        current_spread = max_funding - min_funding;

        if (current_spread < config_.exit_spread_threshold) {
            decision.should_exit = true;
            decision.urgency = 0.7;
            decision.trigger = FailureMode::SPREAD_COLLAPSE;
            decision.reason = fmt::format(
                "SPREAD COLLAPSED: {:.4f}% < {:.4f}% threshold",
                current_spread * 100, config_.exit_spread_threshold * 100
            );
            return decision;
        }

        // KILL CONDITION 2: Funding flipped on primary positions
        // (If we're short on an exchange, we want positive funding there)
        for (const auto& [exchange, pos] : current_position.positions) {
            auto rate = history.current_rate(exchange);
            if (!rate) continue;

            // We're short (pos < 0) but funding turned negative = we now PAY
            if (pos < 0 && *rate < -0.0001) {
                decision.should_exit = true;
                decision.urgency = 0.8;
                decision.trigger = FailureMode::FUNDING_FLIP;
                decision.reason = fmt::format(
                    "FUNDING FLIPPED on {}: rate {:.4f}%, we're SHORT",
                    exchange, *rate * 100
                );
                return decision;
            }

            // We're long (pos > 0) but funding turned very positive = we PAY a lot
            if (pos > 0 && *rate > 0.0005) {  // 0.05% is high
                decision.should_exit = true;
                decision.urgency = 0.6;
                decision.trigger = FailureMode::FUNDING_FLIP;
                decision.reason = fmt::format(
                    "FUNDING ELEVATED on {}: rate {:.4f}%, we're LONG",
                    exchange, *rate * 100
                );
                return decision;
            }
        }

        // KILL CONDITION 3: Drawdown limit
        if (current_pnl_fraction < -config_.max_drawdown) {
            decision.should_exit = true;
            decision.urgency = 0.9;
            decision.trigger = FailureMode::BASIS_DIVERGENCE;
            decision.reason = fmt::format(
                "MAX DRAWDOWN: {:.1f}% loss exceeds {:.1f}% limit",
                -current_pnl_fraction * 100, config_.max_drawdown * 100
            );
            return decision;
        }

        decision.should_exit = false;
        decision.reason = "Position healthy";
        return decision;
    }

private:
    Config config_;
};

// ============================================================================
// PART 5: POSITION SIZING (KELLY-OPTIMAL UNDER CONSTRAINTS)
//
// f* = (μ - r) / σ² is the classic Kelly formula
// We use HALF-KELLY for safety and adjust for exchange risk
// ============================================================================

class KellyPositionSizer {
public:
    struct Inputs {
        double expected_return;      // Per period
        double volatility;           // Per period
        double periods_per_year;     // 3 * 365 for 8h funding
        double risk_free_rate;       // Annual
        double exchange_failure_prob; // Annual probability
        double loss_given_failure;   // Fraction lost if failure
    };

    // Optimal position size as fraction of capital
    static double optimal_fraction(const Inputs& in, double kelly_multiplier = 0.5) {
        // Annualize
        double annual_return = in.expected_return * in.periods_per_year;
        double annual_vol = in.volatility * std::sqrt(in.periods_per_year);

        // Adjust for exchange risk
        double adjusted_return = annual_return - in.exchange_failure_prob * in.loss_given_failure;

        // Classic Kelly
        double excess_return = adjusted_return - in.risk_free_rate;
        if (annual_vol < 1e-10 || excess_return <= 0) return 0;

        double kelly = excess_return / (annual_vol * annual_vol);

        // Apply multiplier and cap
        double fraction = kelly * kelly_multiplier;
        fraction = std::min(fraction, 2.0);  // Max 2x leverage
        fraction = std::max(fraction, 0.0);

        return fraction;
    }

    // Distribution across N exchanges
    static std::map<std::string, double> distribute(
        double total_fraction,
        const std::vector<std::pair<std::string, double>>& exchange_scores,
        double max_per_exchange = 0.25
    ) {
        std::map<std::string, double> result;

        // Normalize scores
        double total_score = 0;
        for (const auto& [ex, score] : exchange_scores) {
            total_score += std::abs(score);
        }

        if (total_score < 1e-10) return result;

        // Allocate proportionally, respecting caps
        double remaining = total_fraction;
        for (const auto& [ex, score] : exchange_scores) {
            double ideal = total_fraction * std::abs(score) / total_score;
            double actual = std::min(ideal, max_per_exchange);
            actual = std::min(actual, remaining);
            result[ex] = actual * (score > 0 ? 1.0 : -1.0);
            remaining -= actual;
        }

        return result;
    }
};

}  // namespace arb
