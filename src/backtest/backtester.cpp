#include "backtest/backtester.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <thread>
#include <chrono>

namespace arb {
namespace backtest {

// ============================================================================
// HTTP helper (standalone, no signing needed for /info)
// ============================================================================
static size_t write_cb(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

static nlohmann::json http_post(const std::string& url, const nlohmann::json& body) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string response;
    std::string body_str = body.dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(fmt::format("HTTP POST failed: {}", curl_easy_strerror(res)));
    }

    return nlohmann::json::parse(response);
}

// ============================================================================
// Backtester
// ============================================================================
Backtester::Backtester(const Config& config, double starting_capital)
    : config_(config), starting_capital_(starting_capital)
{}

std::vector<FundingRecord> Backtester::fetch_funding_history(
    const std::string& coin, int64_t start_time_ms, int64_t end_time_ms)
{
    std::vector<FundingRecord> all_records;
    std::string url = "https://api.hyperliquid.xyz/info";
    int64_t cursor = start_time_ms;

    spdlog::info("Fetching funding history for {} from {} to {}...", coin, start_time_ms, end_time_ms);

    while (cursor < end_time_ms) {
        nlohmann::json body = {
            {"type", "fundingHistory"},
            {"coin", coin},
            {"startTime", cursor}
        };

        auto response = http_post(url, body);

        if (!response.is_array() || response.empty()) {
            break;
        }

        for (const auto& record : response) {
            FundingRecord fr;
            fr.coin = record.value("coin", coin);
            fr.funding_rate = std::stod(record.value("fundingRate", "0"));
            fr.premium = std::stod(record.value("premium", "0"));
            fr.time_ms = record.value("time", int64_t(0));

            if (fr.time_ms <= end_time_ms) {
                all_records.push_back(fr);
            }
        }

        // Pagination: use last record's time + 1 as next cursor
        int64_t last_time = response.back().value("time", int64_t(0));
        if (last_time <= cursor) {
            break;  // No progress, stop
        }
        cursor = last_time + 1;

        // Rate limiting: ~20 requests per second is fine
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (response.size() < 500) {
            break;  // Less than max results = we've caught up
        }
    }

    spdlog::info("Fetched {} funding records for {}", all_records.size(), coin);
    return all_records;
}

BacktestResult Backtester::run(const std::string& symbol,
                                const std::vector<FundingRecord>& history)
{
    if (history.empty()) {
        spdlog::warn("No historical data to backtest");
        return {};
    }

    BacktestResult result;
    BacktestPosition pos;
    double capital = starting_capital_;
    double peak_capital = capital;
    double max_drawdown = 0;
    std::vector<double> hourly_returns;

    double allocation_pct = config_.capital.allocation_pct / 100.0;
    double perp_taker_fee = config_.fees.perp_taker_fee_pct;
    int min_lookback = std::min(static_cast<int>(config_.strategy.funding_rate_lookback_periods),
                                 static_cast<int>(history.size()));
    double min_funding_rate = config_.strategy.min_funding_rate;
    double min_annual_yield = config_.strategy.min_annual_yield_pct;
    int funding_periods = config_.strategy.funding_periods_per_day;
    double min_notional = config_.strategy.min_notional_usd;
    int max_hold = config_.strategy.max_holding_periods;

    spdlog::info("Starting backtest: capital=${:.2f}, alloc={:.0f}%, fee={:.4f}%, periods/day={}",
                 capital, allocation_pct * 100, perp_taker_fee * 100, funding_periods);
    spdlog::info("Entry threshold: min_rate={:.6f}, min_APY={:.1f}%, lookback={}",
                 min_funding_rate, min_annual_yield, min_lookback);

    int64_t prev_day = 0;

    for (size_t i = 0; i < history.size(); i++) {
        const auto& fr = history[i];
        double rate = fr.funding_rate;
        double hourly_pnl = 0;

        // Compute rolling average
        double avg_rate = compute_average(history, i, min_lookback);
        double ema_rate = compute_ema(history, i, min_lookback);

        if (pos.is_open) {
            // Collect funding: when rate > 0, shorts receive payment
            // Payment = position_notional * rate
            double funding_payment = pos.notional * rate;
            pos.funding_collected += funding_payment;
            pos.funding_periods++;
            hourly_pnl = funding_payment;

            // Check exit conditions
            if (should_exit(pos, history, i, avg_rate)) {
                // Close position - pay exit fee
                double exit_fee = pos.notional * perp_taker_fee;
                pos.fees_paid += exit_fee;

                double trade_pnl = pos.funding_collected - pos.fees_paid;
                capital += trade_pnl;

                result.total_entries++;
                result.total_exits++;
                if (trade_pnl > 0) result.profitable_trades++;
                result.avg_hold_hours += pos.funding_periods;

                spdlog::debug("CLOSED {} after {}h: funding=${:.4f} fees=${:.4f} net=${:.4f}",
                             symbol, pos.funding_periods,
                             pos.funding_collected, pos.fees_paid, trade_pnl);

                pos = {};  // Reset position
            }
        } else {
            // Check entry conditions
            if (i >= static_cast<size_t>(min_lookback) &&
                should_enter(history, i, avg_rate)) {

                double available = capital * allocation_pct;
                // Respect exposure limits
                available = std::min(available, config_.risk.max_per_symbol_exposure_usd);
                available = std::min(available, config_.risk.max_total_exposure_usd);

                if (available >= min_notional) {
                    pos.symbol = symbol;
                    pos.notional = available;
                    pos.size = available;  // Simplified: 1 USD = 1 unit
                    pos.entry_price = 1.0;
                    pos.is_open = true;
                    pos.open_time_ms = fr.time_ms;
                    pos.funding_collected = 0;
                    pos.funding_periods = 0;

                    // Entry fee
                    double entry_fee = available * perp_taker_fee;
                    pos.fees_paid = entry_fee;

                    result.total_entries++;

                    spdlog::debug("OPENED {} notional=${:.2f} at rate={:.6f} avg={:.6f}",
                                 symbol, available, rate, avg_rate);
                }
            }
        }

        // Track metrics
        hourly_returns.push_back(hourly_pnl);
        if (pos.is_open) result.hours_in_position++;

        // Track drawdown
        if (capital > peak_capital) peak_capital = capital;
        double dd = (peak_capital > 0) ? (peak_capital - capital) / peak_capital * 100.0 : 0;
        if (dd > max_drawdown) max_drawdown = dd;

        // Daily snapshot (every 24 hours)
        int64_t day = fr.time_ms / (24 * 3600 * 1000);
        if (day != prev_day) {
            BacktestResult::DailySnapshot snap;
            snap.time_ms = fr.time_ms;
            snap.cumulative_pnl = capital - starting_capital_;
            snap.funding_rate = rate;
            snap.in_position = pos.is_open;
            result.daily_series.push_back(snap);
            prev_day = day;
        }
    }

    // If still in position at end, calculate unrealized PnL
    if (pos.is_open) {
        double exit_fee = pos.notional * perp_taker_fee;
        double trade_pnl = pos.funding_collected - pos.fees_paid - exit_fee;
        capital += trade_pnl;
        result.avg_hold_hours += pos.funding_periods;
        result.total_exits++;
        if (trade_pnl > 0) result.profitable_trades++;
    }

    // Compute results
    result.total_pnl = capital - starting_capital_;
    result.total_funding_earned = std::accumulate(hourly_returns.begin(), hourly_returns.end(), 0.0);
    result.total_fees_paid = result.total_funding_earned - result.total_pnl;
    result.net_profit = result.total_pnl;
    result.max_drawdown_pct = max_drawdown;
    result.total_hours = static_cast<int>(history.size());
    result.utilization_pct = (result.total_hours > 0)
        ? static_cast<double>(result.hours_in_position) / result.total_hours * 100.0 : 0;

    // Annualized return
    double years = static_cast<double>(result.total_hours) / (24.0 * 365.0);
    if (years > 0 && starting_capital_ > 0) {
        result.annualized_return_pct = (result.total_pnl / starting_capital_) / years * 100.0;
    }

    // Sharpe ratio (annualized, hourly returns)
    if (!hourly_returns.empty()) {
        double mean = std::accumulate(hourly_returns.begin(), hourly_returns.end(), 0.0) / hourly_returns.size();
        double sq_sum = 0;
        for (double r : hourly_returns) sq_sum += (r - mean) * (r - mean);
        double stddev = std::sqrt(sq_sum / hourly_returns.size());
        if (stddev > 0) {
            result.sharpe_ratio = (mean / stddev) * std::sqrt(24.0 * 365.0);  // Annualize
        }
    }

    // Average hold time
    if (result.total_exits > 0) {
        result.avg_hold_hours /= result.total_exits;
    }

    return result;
}

BacktestResult Backtester::run_live(const std::string& symbol, int days_back) {
    // Strip suffix to get coin name
    std::string coin = symbol;
    for (const auto& suffix : {"USDT", "USDC", "USD", "PERP"}) {
        size_t pos = coin.rfind(suffix);
        if (pos != std::string::npos && pos + std::strlen(suffix) == coin.size()) {
            coin = coin.substr(0, pos);
            break;
        }
    }

    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    auto start_ms = now_ms - (static_cast<int64_t>(days_back) * 24 * 3600 * 1000);

    auto history = fetch_funding_history(coin, start_ms, now_ms);
    return run(symbol, history);
}

bool Backtester::should_enter(const std::vector<FundingRecord>& history,
                               size_t current_idx, double avg_rate) const
{
    double rate = history[current_idx].funding_rate;

    // Rate must be positive (shorts earn)
    if (rate <= 0) return false;

    // Must exceed minimum per-period rate
    if (rate < config_.strategy.min_funding_rate) return false;

    // Average must also be positive
    if (avg_rate < config_.strategy.min_avg_funding_rate) return false;

    // Annualized yield check
    double annual = rate * config_.strategy.funding_periods_per_day * 365.0 * 100.0;
    if (annual < config_.strategy.min_annual_yield_pct) return false;

    // Fee breakeven check: can we earn back entry+exit fees within max_fee_breakeven_days?
    double round_trip_fee = 2.0 * config_.fees.perp_taker_fee_pct;
    double hourly_earning = avg_rate;  // Per-hour rate
    double hours_to_breakeven = (hourly_earning > 0) ? round_trip_fee / hourly_earning : 1e9;
    double days_to_breakeven = hours_to_breakeven / 24.0;
    if (days_to_breakeven > config_.strategy.max_fee_breakeven_days) return false;

    // EMA check: current rate should be above EMA (trend confirmation)
    double ema = compute_ema(history, current_idx,
                              config_.strategy.funding_rate_lookback_periods);
    if (rate < ema * 0.8) return false;  // Allow 20% below EMA

    return true;
}

bool Backtester::should_exit(const BacktestPosition& pos,
                              const std::vector<FundingRecord>& history,
                              size_t current_idx, double avg_rate) const
{
    double rate = history[current_idx].funding_rate;

    // Exit if funding turned negative (we'd be paying)
    if (rate < config_.risk.max_funding_rate_to_pay) return true;

    // Exit if held too long
    if (pos.funding_periods >= config_.strategy.max_holding_periods) return true;

    // Exit if average has dropped below entry threshold
    if (avg_rate < config_.strategy.min_avg_funding_rate * 0.5) return true;

    // Exit if cumulative fees exceed cumulative funding (position is net negative)
    if (pos.funding_periods > 48 && pos.funding_collected < pos.fees_paid) return true;

    return false;
}

double Backtester::compute_ema(const std::vector<FundingRecord>& history,
                                size_t end_idx, int lookback, double alpha) const
{
    if (history.empty() || end_idx == 0) return 0;

    size_t start = (end_idx >= static_cast<size_t>(lookback))
                    ? end_idx - lookback : 0;

    double ema = history[start].funding_rate;
    for (size_t i = start + 1; i <= end_idx; i++) {
        ema = alpha * history[i].funding_rate + (1.0 - alpha) * ema;
    }
    return ema;
}

double Backtester::compute_average(const std::vector<FundingRecord>& history,
                                    size_t end_idx, int lookback) const
{
    if (history.empty()) return 0;

    size_t start = (end_idx >= static_cast<size_t>(lookback))
                    ? end_idx - lookback : 0;
    double sum = 0;
    int count = 0;
    for (size_t i = start; i <= end_idx; i++) {
        sum += history[i].funding_rate;
        count++;
    }
    return (count > 0) ? sum / count : 0;
}

// ============================================================================
// Report
// ============================================================================
void BacktestResult::print_report() const {
    std::cout << "\n";
    std::cout << "================================================================\n";
    std::cout << "                    BACKTEST RESULTS\n";
    std::cout << "================================================================\n";
    std::cout << "\n";
    std::cout << "  PERFORMANCE\n";
    std::cout << "  -------------------------------------------------------\n";
    std::cout << fmt::format("  Net Profit:          ${:.4f}\n", net_profit);
    std::cout << fmt::format("  Funding Earned:      ${:.4f}\n", total_funding_earned);
    std::cout << fmt::format("  Total Fees:          ${:.4f}\n", total_fees_paid);
    std::cout << fmt::format("  Annualized Return:   {:.2f}%\n", annualized_return_pct);
    std::cout << fmt::format("  Max Drawdown:        {:.2f}%\n", max_drawdown_pct);
    std::cout << fmt::format("  Sharpe Ratio:        {:.2f}\n", sharpe_ratio);
    std::cout << "\n";
    std::cout << "  TIMING\n";
    std::cout << "  -------------------------------------------------------\n";
    std::cout << fmt::format("  Total Hours:         {}\n", total_hours);
    std::cout << fmt::format("  Total Days:          {:.1f}\n", total_hours / 24.0);
    std::cout << fmt::format("  Hours in Position:   {}\n", hours_in_position);
    std::cout << fmt::format("  Utilization:         {:.1f}%\n", utilization_pct);
    std::cout << "\n";
    std::cout << "  TRADES\n";
    std::cout << "  -------------------------------------------------------\n";
    std::cout << fmt::format("  Total Entries:       {}\n", total_entries);
    std::cout << fmt::format("  Total Exits:         {}\n", total_exits);
    std::cout << fmt::format("  Profitable Trades:   {}/{}\n", profitable_trades, total_exits);
    std::cout << fmt::format("  Win Rate:            {:.1f}%\n",
                              total_exits > 0 ? static_cast<double>(profitable_trades) / total_exits * 100 : 0);
    std::cout << fmt::format("  Avg Hold Time:       {:.1f} hours\n", avg_hold_hours);
    std::cout << "\n";

    // ASCII PnL chart
    if (!daily_series.empty()) {
        std::cout << "  DAILY PnL CURVE\n";
        std::cout << "  -------------------------------------------------------\n";

        double min_pnl = 0, max_pnl = 0;
        for (const auto& s : daily_series) {
            min_pnl = std::min(min_pnl, s.cumulative_pnl);
            max_pnl = std::max(max_pnl, s.cumulative_pnl);
        }
        double range = max_pnl - min_pnl;
        if (range < 0.001) range = 1.0;

        int chart_width = 50;
        // Sample ~30 points for display
        size_t step = std::max(size_t(1), daily_series.size() / 30);
        for (size_t i = 0; i < daily_series.size(); i += step) {
            double normalized = (daily_series[i].cumulative_pnl - min_pnl) / range;
            int bar_len = static_cast<int>(normalized * chart_width);
            std::string bar(bar_len, '#');
            std::cout << fmt::format("  {:>8.4f} |{}\n", daily_series[i].cumulative_pnl, bar);
        }
    }

    std::cout << "================================================================\n\n";
}

} // namespace backtest
} // namespace arb
