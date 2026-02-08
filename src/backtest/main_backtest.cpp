#include <iostream>
#include <string>
#include <chrono>
#include <random>
#include <fstream>

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "config/config.hpp"
#include "backtest/backtester.hpp"

using namespace arb;

/**
 * Generate synthetic funding rate data based on real Hyperliquid statistics.
 *
 * Real-world data characteristics (from Hyperliquid ETH, 2024-2025):
 * - Mean hourly rate: ~0.0001% to 0.003% (varies by market regime)
 * - Standard deviation: ~0.002-0.005%
 * - Funding is mean-reverting with autocorrelation ~0.85
 * - Occasional spikes during high volatility (up to 0.05%)
 * - Can go negative during bearish periods
 *
 * We model this as an Ornstein-Uhlenbeck process (mean-reverting):
 *   rate(t+1) = rate(t) + theta * (mu - rate(t)) + sigma * noise
 */
std::vector<backtest::FundingRecord> generate_synthetic_data(
    const std::string& coin, int days, const std::string& scenario)
{
    std::vector<backtest::FundingRecord> records;
    int total_hours = days * 24;

    // Scenario parameters
    double mu;          // Long-run mean (hourly rate)
    double theta;       // Mean reversion speed
    double sigma;       // Volatility of rate changes
    double spike_prob;  // Probability of a spike per hour
    double spike_mag;   // Magnitude of spike

    if (scenario == "bull") {
        // Bull market: high, stable funding rates
        mu = 0.000080;     // ~0.008% per hour = ~70% APY
        theta = 0.05;
        sigma = 0.000030;
        spike_prob = 0.005;
        spike_mag = 0.0005;
    } else if (scenario == "bear") {
        // Bear market: low, often negative funding
        mu = -0.000010;    // Slightly negative
        theta = 0.03;
        sigma = 0.000050;
        spike_prob = 0.01;
        spike_mag = 0.0003;
    } else if (scenario == "mixed") {
        // Mixed: alternating bull/bear regimes
        mu = 0.000030;     // ~0.003% per hour = ~26% APY
        theta = 0.02;
        sigma = 0.000060;
        spike_prob = 0.008;
        spike_mag = 0.0004;
    } else {
        // "realistic": based on actual ETH averages
        mu = 0.000040;     // ~0.004% per hour = ~35% APY
        theta = 0.04;
        sigma = 0.000040;
        spike_prob = 0.006;
        spike_mag = 0.0003;
    }

    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::normal_distribution<double> noise(0, 1);
    std::uniform_real_distribution<double> uniform(0, 1);

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t start_ms = now_ms - (static_cast<int64_t>(days) * 24 * 3600 * 1000);

    double rate = mu;  // Start at mean

    for (int h = 0; h < total_hours; h++) {
        // For "mixed" scenario, shift mean every ~30 days
        double current_mu = mu;
        if (scenario == "mixed") {
            int regime = (h / (30 * 24)) % 3;
            if (regime == 0) current_mu = 0.000060;       // Bull phase
            else if (regime == 1) current_mu = -0.000010;  // Bear phase
            else current_mu = 0.000030;                     // Neutral phase
        }

        // Ornstein-Uhlenbeck step
        rate = rate + theta * (current_mu - rate) + sigma * noise(rng);

        // Random spike
        if (uniform(rng) < spike_prob) {
            rate += spike_mag * (uniform(rng) > 0.3 ? 1 : -1);
        }

        backtest::FundingRecord fr;
        fr.coin = coin;
        fr.funding_rate = rate;
        fr.premium = rate * 0.8;
        fr.time_ms = start_ms + (static_cast<int64_t>(h) * 3600 * 1000);
        records.push_back(fr);
    }

    return records;
}

/**
 * Load funding rates from a CSV file.
 * Format: timestamp_ms,funding_rate[,premium]
 */
std::vector<backtest::FundingRecord> load_from_csv(
    const std::string& coin, const std::string& path)
{
    std::vector<backtest::FundingRecord> records;
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    std::string line;
    // Skip header if present
    if (std::getline(file, line) && !line.empty() && !std::isdigit(line[0])) {
        // Header line, skip
    } else {
        file.seekg(0);  // Reset to beginning
    }

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        backtest::FundingRecord fr;
        fr.coin = coin;

        size_t pos1 = line.find(',');
        if (pos1 == std::string::npos) continue;

        fr.time_ms = std::stoll(line.substr(0, pos1));

        size_t pos2 = line.find(',', pos1 + 1);
        if (pos2 == std::string::npos) {
            fr.funding_rate = std::stod(line.substr(pos1 + 1));
        } else {
            fr.funding_rate = std::stod(line.substr(pos1 + 1, pos2 - pos1 - 1));
            fr.premium = std::stod(line.substr(pos2 + 1));
        }

        records.push_back(fr);
    }

    return records;
}

int main(int argc, char* argv[]) {
    CLI::App app{"Delta-Neutral Funding Arbitrage Backtester"};

    std::string config_path = "configs/hyperliquid_50usd.json";
    std::string coin = "ETH";
    int days = 90;
    double capital = 0;  // 0 = use config value
    bool verbose = false;
    std::string scenario = "realistic";
    std::string csv_file = "";
    bool use_synthetic = false;
    bool run_all_scenarios = false;

    app.add_option("-c,--config", config_path, "Path to configuration file");
    app.add_option("--coin", coin, "Coin to backtest (e.g., ETH, BTC, SOL)");
    app.add_option("-d,--days", days, "Number of days to look back");
    app.add_option("--capital", capital, "Starting capital (overrides config)");
    app.add_flag("-v,--verbose", verbose, "Verbose output");
    app.add_option("--scenario", scenario,
                   "Synthetic scenario: realistic, bull, bear, mixed");
    app.add_option("--file", csv_file, "Load funding rates from CSV file");
    app.add_flag("--synthetic", use_synthetic,
                 "Use synthetic data (auto-enabled if API unreachable)");
    app.add_flag("--all-scenarios", run_all_scenarios,
                 "Run all synthetic scenarios for comparison");

    CLI11_PARSE(app, argc, argv);

    // Setup logging
    auto console = spdlog::stdout_color_mt("backtest");
    spdlog::set_default_logger(console);
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    // Load config
    Config config;
    try {
        if (std::filesystem::exists(config_path)) {
            config = Config::load(config_path);
            spdlog::info("Loaded config: {}", config_path);
        } else {
            spdlog::info("Config not found, using defaults");
        }
    } catch (const std::exception& e) {
        spdlog::warn("Failed to load config: {} (using defaults)", e.what());
    }

    double starting_capital = (capital > 0) ? capital : config.capital.starting_balance_usd;

    auto print_banner = [&](const std::string& data_source) {
        std::cout << "\n";
        std::cout << "========================================\n";
        std::cout << "  FUNDING RATE ARBITRAGE BACKTESTER\n";
        std::cout << "========================================\n";
        std::cout << "  Coin:     " << coin << "\n";
        std::cout << "  Days:     " << days << "\n";
        std::cout << "  Capital:  $" << starting_capital << "\n";
        std::cout << "  Mode:     " << arb::to_string(config.strategy.hedge_mode) << "\n";
        std::cout << "  Periods:  " << config.strategy.funding_periods_per_day << "/day\n";
        std::cout << "  Min Rate: " << config.strategy.min_funding_rate * 100 << "%\n";
        std::cout << "  Min APY:  " << config.strategy.min_annual_yield_pct << "%\n";
        std::cout << "  Fee:      " << config.fees.perp_taker_fee_pct * 100 << "% taker\n";
        std::cout << "  Data:     " << data_source << "\n";
        std::cout << "========================================\n\n";
    };

    backtest::Backtester bt(config, starting_capital);

    if (run_all_scenarios) {
        // Run all scenarios and compare
        std::cout << "\n";
        std::cout << "================================================================\n";
        std::cout << "      MULTI-SCENARIO COMPARISON (Synthetic Data, " << days << " days)\n";
        std::cout << "================================================================\n";
        std::cout << fmt::format("  Capital: ${:.0f} | Fee: {:.3f}% | Min APY: {:.0f}%\n",
                                 starting_capital, config.fees.perp_taker_fee_pct * 100,
                                 config.strategy.min_annual_yield_pct);
        std::cout << "================================================================\n\n";

        std::cout << fmt::format("  {:<12} {:>10} {:>10} {:>10} {:>8} {:>8} {:>8}\n",
                                 "Scenario", "Net P/L", "Funding", "Fees", "APY%", "DD%", "Sharpe");
        std::cout << "  " << std::string(68, '-') << "\n";

        for (const auto& sc : {"realistic", "bull", "bear", "mixed"}) {
            std::string symbol = coin + "USDT";
            auto data = generate_synthetic_data(coin, days, sc);
            auto result = bt.run(symbol, data);

            std::cout << fmt::format("  {:<12} ${:>8.2f} ${:>8.2f} ${:>8.2f} {:>7.1f} {:>7.2f} {:>7.2f}\n",
                                     sc, result.net_profit, result.total_funding_earned,
                                     result.total_fees_paid, result.annualized_return_pct,
                                     result.max_drawdown_pct, result.sharpe_ratio);
        }

        std::cout << "\n";
        std::cout << "  SCALING PROJECTIONS (realistic scenario)\n";
        std::cout << "  " << std::string(68, '-') << "\n";
        auto realistic_data = generate_synthetic_data(coin, days, "realistic");
        auto base_result = bt.run(coin + "USDT", realistic_data);
        double ror = (starting_capital > 0) ? base_result.net_profit / starting_capital : 0;
        for (double cap : {50.0, 100.0, 250.0, 500.0, 1000.0, 5000.0}) {
            std::cout << fmt::format("  ${:>6.0f} -> ${:>8.2f}/{}d (${:.2f}/day, {:.1f}% APY)\n",
                                     cap, cap * ror, days, cap * ror / days,
                                     base_result.annualized_return_pct);
        }
        std::cout << "\n";

        return 0;
    }

    // Single scenario run
    std::string symbol = coin + "USDT";
    std::vector<backtest::FundingRecord> history;
    std::string data_source;

    if (!csv_file.empty()) {
        data_source = "CSV: " + csv_file;
        history = load_from_csv(coin, csv_file);
    } else if (use_synthetic) {
        data_source = "Synthetic (" + scenario + ")";
        history = generate_synthetic_data(coin, days, scenario);
    } else {
        // Try live API first, fallback to synthetic
        try {
            data_source = "Hyperliquid API (live)";
            print_banner(data_source);
            auto result = bt.run_live(symbol, days);
            result.print_report();
            return 0;
        } catch (const std::exception& e) {
            spdlog::warn("API fetch failed: {} - falling back to synthetic data", e.what());
            data_source = "Synthetic (" + scenario + ") [API fallback]";
            history = generate_synthetic_data(coin, days, scenario);
        }
    }

    print_banner(data_source);
    auto result = bt.run(symbol, history);
    result.print_report();

    // Scaling projections
    if (starting_capital <= 100) {
        std::cout << "  SCALING PROJECTIONS (same conditions)\n";
        std::cout << "  -------------------------------------------------------\n";
        double rate_of_return = (starting_capital > 0)
            ? result.net_profit / starting_capital : 0;
        for (double cap : {50.0, 100.0, 250.0, 500.0, 1000.0}) {
            double projected = cap * rate_of_return;
            std::cout << fmt::format("  ${:>6.0f} -> ${:.2f}/{}d = {:.1f}% APY\n",
                                     cap, projected, days, result.annualized_return_pct);
        }
        std::cout << "\n";
    }

    return 0;
}
