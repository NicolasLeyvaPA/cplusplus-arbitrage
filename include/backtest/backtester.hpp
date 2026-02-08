#pragma once

#include "common/types.hpp"
#include "config/config.hpp"
#include <vector>
#include <string>
#include <map>
#include <functional>

namespace arb {
namespace backtest {

struct FundingRecord {
    std::string coin;
    double funding_rate{0};
    double premium{0};
    int64_t time_ms{0};
};

struct BacktestPosition {
    std::string symbol;
    double size{0};           // In coin units
    double entry_price{0};
    double notional{0};       // USD value at entry
    double funding_collected{0};
    double fees_paid{0};
    int funding_periods{0};
    int64_t open_time_ms{0};
    bool is_open{false};
};

struct BacktestResult {
    // Summary
    double total_pnl{0};
    double total_funding_earned{0};
    double total_fees_paid{0};
    double net_profit{0};
    double annualized_return_pct{0};
    double max_drawdown_pct{0};
    double sharpe_ratio{0};

    // Timing
    int total_hours{0};
    int hours_in_position{0};
    double utilization_pct{0};

    // Trades
    int total_entries{0};
    int total_exits{0};
    int profitable_trades{0};
    double avg_hold_hours{0};

    // Daily PnL series for charting
    struct DailySnapshot {
        int64_t time_ms{0};
        double cumulative_pnl{0};
        double funding_rate{0};
        bool in_position{false};
    };
    std::vector<DailySnapshot> daily_series;

    void print_report() const;
};

class Backtester {
public:
    Backtester(const Config& config, double starting_capital);

    // Fetch historical data from Hyperliquid API
    std::vector<FundingRecord> fetch_funding_history(
        const std::string& coin,
        int64_t start_time_ms,
        int64_t end_time_ms);

    // Run backtest on historical data
    BacktestResult run(const std::string& symbol,
                       const std::vector<FundingRecord>& history);

    // Run backtest fetching data automatically for last N days
    BacktestResult run_live(const std::string& symbol, int days_back = 90);

private:
    // Strategy decision functions
    bool should_enter(const std::vector<FundingRecord>& history,
                      size_t current_idx,
                      double avg_rate) const;

    bool should_exit(const BacktestPosition& pos,
                     const std::vector<FundingRecord>& history,
                     size_t current_idx,
                     double avg_rate) const;

    double compute_ema(const std::vector<FundingRecord>& history,
                       size_t end_idx, int lookback, double alpha = 0.3) const;

    double compute_average(const std::vector<FundingRecord>& history,
                           size_t end_idx, int lookback) const;

    Config config_;
    double starting_capital_;
};

} // namespace backtest
} // namespace arb
