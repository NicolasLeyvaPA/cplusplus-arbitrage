#include "strategy/delta_neutral_engine.hpp"
#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>

namespace arb {

DeltaNeutralEngine::DeltaNeutralEngine(FundingMonitor& funding,
                                       BasisCalculator& basis,
                                       const StrategyConfig& strategy_config,
                                       const RiskConfig& risk_config)
    : funding_(funding)
    , basis_(basis)
    , strategy_config_(strategy_config)
    , risk_config_(risk_config)
{}

std::vector<DeltaNeutralSignal> DeltaNeutralEngine::evaluate(
    const std::map<std::string, DeltaNeutralPosition>& positions,
    double available_capital)
{
    std::vector<DeltaNeutralSignal> signals;

    for (const auto& symbol : strategy_config_.symbols) {
        auto pos_it = positions.find(symbol);
        bool has_position = pos_it != positions.end() && pos_it->second.is_open();

        if (has_position) {
            // Check if we should exit
            auto exit_signal = evaluate_exit(symbol, pos_it->second);
            if (exit_signal) {
                signals.push_back(*exit_signal);
                continue;
            }

            // Check if we need to rebalance
            auto rebalance_signal = evaluate_rebalance(symbol, pos_it->second);
            if (rebalance_signal) {
                signals.push_back(*rebalance_signal);
            }
        } else {
            // Check if we should enter
            int open_count = 0;
            for (const auto& [s, p] : positions) {
                if (p.is_open()) open_count++;
            }

            if (open_count < strategy_config_.max_positions) {
                auto entry_signal = evaluate_entry(symbol, available_capital);
                if (entry_signal) {
                    signals.push_back(*entry_signal);
                }
            }
        }
    }

    return signals;
}

std::optional<DeltaNeutralSignal> DeltaNeutralEngine::evaluate_entry(
    const std::string& symbol, double available_capital)
{
    auto funding_info = funding_.get_funding(symbol);

    // Check minimum funding rate
    if (funding_info.current_rate < strategy_config_.min_funding_rate) {
        return std::nullopt;
    }

    // Check annualized yield
    if (funding_info.annualized_rate() < strategy_config_.min_annual_yield_pct) {
        return std::nullopt;
    }

    // Check average funding rate
    double avg_rate = funding_.average_funding_rate(symbol, strategy_config_.funding_rate_lookback_periods);
    if (avg_rate < strategy_config_.min_avg_funding_rate) {
        return std::nullopt;
    }

    // Check basis
    auto basis_info = basis_.get_basis(symbol);
    if (std::abs(basis_info.basis) * 100.0 > strategy_config_.max_basis_deviation_pct) {
        spdlog::debug("{} basis too wide: {:.4f}%", symbol, basis_info.basis * 100.0);
        return std::nullopt;
    }

    // Compute opportunity score
    auto opportunities = funding_.get_ranked_opportunities();
    double score = 0;
    for (const auto& opp : opportunities) {
        if (opp.symbol == symbol) {
            score = opp.score;
            break;
        }
    }

    if (score < strategy_config_.entry_score_threshold) {
        return std::nullopt;
    }

    // Calculate position size
    Price price = basis_info.spot_mid > 0 ? basis_info.spot_mid : funding_info.mark_price;
    Size size = calculate_position_size(symbol, available_capital, price);
    if (size <= 0) return std::nullopt;

    DeltaNeutralSignal signal;
    signal.type = SignalType::OPEN;
    signal.symbol = symbol;
    signal.target_size = size;
    signal.expected_funding_rate = funding_info.current_rate;
    signal.expected_annual_yield = funding_info.annualized_rate();
    signal.confidence = score;
    signal.reason = fmt::format("Funding {:.4f}% ({:.1f}% APY), avg {:.4f}%, basis {:.2f}bps, score {:.2f}",
                                 funding_info.current_rate * 100,
                                 funding_info.annualized_rate(),
                                 avg_rate * 100,
                                 basis_info.basis_bps,
                                 score);

    spdlog::info("ENTRY signal: {} size={:.6f} | {}", symbol, size, signal.reason);
    return signal;
}

std::optional<DeltaNeutralSignal> DeltaNeutralEngine::evaluate_exit(
    const std::string& symbol, const DeltaNeutralPosition& position)
{
    auto funding_info = funding_.get_funding(symbol);
    auto basis_info = basis_.get_basis(symbol);

    // Exit if funding turned negative (we'd be paying)
    if (funding_info.current_rate < risk_config_.max_funding_rate_to_pay) {
        DeltaNeutralSignal signal;
        signal.type = SignalType::CLOSE;
        signal.symbol = symbol;
        signal.target_size = position.spot_size;
        signal.reason = fmt::format("Funding turned negative: {:.4f}%", funding_info.current_rate * 100);
        spdlog::info("EXIT signal (funding negative): {} | {}", symbol, signal.reason);
        return signal;
    }

    // Exit if basis risk too high
    if (std::abs(basis_info.basis_bps) > risk_config_.stop_loss_basis_bps) {
        DeltaNeutralSignal signal;
        signal.type = SignalType::CLOSE;
        signal.symbol = symbol;
        signal.target_size = position.spot_size;
        signal.reason = fmt::format("Basis risk: {:.2f}bps exceeds limit {:.0f}bps",
                                     basis_info.basis_bps, risk_config_.stop_loss_basis_bps);
        spdlog::info("EXIT signal (basis risk): {} | {}", symbol, signal.reason);
        return signal;
    }

    // Exit if held too long
    if (position.funding_periods >= strategy_config_.max_holding_periods) {
        DeltaNeutralSignal signal;
        signal.type = SignalType::CLOSE;
        signal.symbol = symbol;
        signal.target_size = position.spot_size;
        signal.reason = fmt::format("Max holding period reached: {} periods", position.funding_periods);
        spdlog::info("EXIT signal (max hold): {} | {}", symbol, signal.reason);
        return signal;
    }

    return std::nullopt;
}

std::optional<DeltaNeutralSignal> DeltaNeutralEngine::evaluate_rebalance(
    const std::string& symbol, const DeltaNeutralPosition& position)
{
    if (!position.is_balanced(strategy_config_.rebalance_threshold_pct)) {
        double imbalance_pct = (position.spot_size > 0)
            ? (std::abs(position.net_delta()) / std::abs(position.spot_size)) * 100.0
            : 0;

        DeltaNeutralSignal signal;
        signal.type = SignalType::REBALANCE;
        signal.symbol = symbol;
        signal.target_size = std::abs(position.net_delta());
        signal.reason = fmt::format("Delta imbalance: {:.2f}% (net delta {:.6f})",
                                     imbalance_pct, position.net_delta());
        spdlog::info("REBALANCE signal: {} | {}", symbol, signal.reason);
        return signal;
    }

    return std::nullopt;
}

Size DeltaNeutralEngine::calculate_position_size(
    const std::string& symbol, double capital, Price price)
{
    if (price <= 0 || capital <= 0) return 0;

    // Use configured allocation percentage
    double usable_capital = capital * 0.3;  // 30% per position max

    // Respect per-symbol exposure limit
    usable_capital = std::min(usable_capital, risk_config_.max_per_symbol_exposure_usd);

    // Respect total exposure limit
    usable_capital = std::min(usable_capital, risk_config_.max_total_exposure_usd / strategy_config_.max_positions);

    Size size = usable_capital / price;

    // Minimum size checks (Binance minimums)
    if (symbol == "BTCUSDT" && size < 0.001) return 0;
    if (symbol == "ETHUSDT" && size < 0.01) return 0;
    if (size * price < 10.0) return 0;  // Min $10 notional

    return size;
}

} // namespace arb
