#pragma once

#include <string>
#include <vector>
#include <optional>
#include "common/types.hpp"
#include "config/config.hpp"
#include "market_data/order_book.hpp"

namespace arb {

/**
 * Base class for all trading strategies.
 * Strategies compute signals based on market data.
 */
class StrategyBase {
public:
    explicit StrategyBase(const std::string& name, const StrategyConfig& config);
    virtual ~StrategyBase() = default;

    // Generate signals based on current market state
    virtual std::vector<Signal> evaluate(
        const BinaryMarketBook& book,
        const BtcPrice& btc_price,
        Timestamp now
    ) = 0;

    // Strategy name
    const std::string& name() const { return name_; }

    // Enable/disable
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool is_enabled() const { return enabled_; }

    // Stats
    int64_t signals_generated() const { return signals_generated_; }
    int64_t signals_acted_on() const { return signals_acted_on_; }
    void record_signal_acted() { ++signals_acted_on_; }

protected:
    std::string name_;
    StrategyConfig config_;
    bool enabled_{true};
    int64_t signals_generated_{0};
    int64_t signals_acted_on_{0};
};

/**
 * Strategy S2: Two-outcome underpricing detection.
 * Identifies when sum of best asks for YES + NO < 1 - fees.
 */
class UnderpricingStrategy : public StrategyBase {
public:
    explicit UnderpricingStrategy(const StrategyConfig& config);

    std::vector<Signal> evaluate(
        const BinaryMarketBook& book,
        const BtcPrice& btc_price,
        Timestamp now
    ) override;

    // Calculate edge after fees
    double calculate_edge(double yes_ask, double no_ask, double fee_rate_bps) const;

    // Check if profitable
    bool is_profitable(double edge) const;

private:
    // Fee calculation constants
    static constexpr double POLYMARKET_FEE_BPS = 200.0;  // 2% fee on winnings
};

/**
 * Strategy S1: Stale-odds / lag arbitrage.
 * Detects when Polymarket odds are stale relative to BTC price movement.
 */
class StaleOddsStrategy : public StrategyBase {
public:
    explicit StaleOddsStrategy(const StrategyConfig& config);

    std::vector<Signal> evaluate(
        const BinaryMarketBook& book,
        const BtcPrice& btc_price,
        Timestamp now
    ) override;

    // Set reference price for staleness detection
    void update_btc_reference(const BtcPrice& price);

    // Calculate implied probability from market prices
    double calculate_implied_prob(double yes_ask, double no_ask) const;

    // Calculate expected probability from BTC move
    double calculate_expected_prob(double btc_move_bps, double current_implied) const;

private:
    BtcPrice reference_btc_;
    Timestamp reference_time_;

    // Track price history for staleness detection
    struct PricePoint {
        Price btc_price;
        Timestamp time;
    };
    std::vector<PricePoint> btc_history_;
    static constexpr size_t MAX_HISTORY = 100;

    double detect_btc_move_bps() const;
    bool is_market_stale(const BinaryMarketBook& book, Timestamp now) const;
};

/**
 * Strategy S3: Market making (optional, conservative).
 */
class MarketMakingStrategy : public StrategyBase {
public:
    explicit MarketMakingStrategy(const StrategyConfig& config);

    std::vector<Signal> evaluate(
        const BinaryMarketBook& book,
        const BtcPrice& btc_price,
        Timestamp now
    ) override;

private:
    // Calculate fair value based on external signals
    double calculate_fair_value(const BinaryMarketBook& book, const BtcPrice& btc_price) const;

    // Calculate quote prices with spread
    std::pair<Price, Price> calculate_quotes(double fair_value, double spread) const;
};

} // namespace arb
