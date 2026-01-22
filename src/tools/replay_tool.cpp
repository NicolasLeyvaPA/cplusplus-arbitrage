#include <iostream>
#include <fstream>
#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "common/types.hpp"
#include "config/config.hpp"
#include "market_data/order_book.hpp"
#include "strategy/strategy_base.hpp"
#include "utils/time_utils.hpp"

using namespace arb;

/**
 * Replay tool for backtesting strategies against recorded market data.
 *
 * Usage:
 *   ./replay_tool --input data/recorded_feed.json --strategy s2
 */

struct ReplayStats {
    int messages_processed{0};
    int signals_generated{0};
    int trades_simulated{0};
    double total_pnl{0.0};
    double total_fees{0.0};
    double max_drawdown{0.0};
    double peak_pnl{0.0};
};

void run_replay(const std::string& input_file, const std::string& strategy_name,
                const StrategyConfig& config, bool verbose) {
    spdlog::info("Starting replay from: {}", input_file);

    // Create strategy
    std::unique_ptr<StrategyBase> strategy;
    if (strategy_name == "s1" || strategy_name == "S1") {
        strategy = std::make_unique<StaleOddsStrategy>(config);
    } else if (strategy_name == "s2" || strategy_name == "S2") {
        strategy = std::make_unique<UnderpricingStrategy>(config);
    } else {
        spdlog::error("Unknown strategy: {}", strategy_name);
        return;
    }

    // Create order books for markets
    std::map<std::string, std::unique_ptr<BinaryMarketBook>> books;

    BtcPrice btc_price;
    ReplayStats stats;

    // Read and process file
    std::ifstream file(input_file);
    if (!file.is_open()) {
        spdlog::error("Failed to open input file: {}", input_file);
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        try {
            auto j = nlohmann::json::parse(line);

            std::string event_type = j.value("type", j.value("event_type", ""));

            if (event_type == "btc_price" || event_type == "binance") {
                // Update BTC price
                btc_price.bid = j.value("bid", btc_price.bid);
                btc_price.ask = j.value("ask", btc_price.ask);
                btc_price.mid = (btc_price.bid + btc_price.ask) / 2.0;
                btc_price.timestamp = now();
            }
            else if (event_type == "book" || event_type == "polymarket") {
                // Update order book
                std::string market_id = j.value("market_id", j.value("condition_id", ""));
                std::string asset_id = j.value("asset_id", j.value("token_id", ""));

                if (market_id.empty()) continue;

                // Create book if needed
                if (books.find(market_id) == books.end()) {
                    books[market_id] = std::make_unique<BinaryMarketBook>(market_id);
                }

                BinaryMarketBook* book = books[market_id].get();

                // Determine if YES or NO
                bool is_yes = j.value("outcome", "") == "YES" ||
                             asset_id.find("yes") != std::string::npos;

                OrderBook* target = is_yes ? &book->yes_book() : &book->no_book();

                // Apply updates
                std::vector<PriceLevel> bids, asks;

                if (j.contains("bids")) {
                    for (const auto& bid : j["bids"]) {
                        PriceLevel level;
                        level.price = bid.value("price", 0.0);
                        level.size = bid.value("size", 0.0);
                        if (level.price > 0) bids.push_back(level);
                    }
                }

                if (j.contains("asks")) {
                    for (const auto& ask : j["asks"]) {
                        PriceLevel level;
                        level.price = ask.value("price", 0.0);
                        level.size = ask.value("size", 0.0);
                        if (level.price > 0) asks.push_back(level);
                    }
                }

                target->apply_snapshot(bids, asks);

                // Evaluate strategy
                if (book->has_liquidity()) {
                    auto signals = strategy->evaluate(*book, btc_price, now());

                    for (const auto& signal : signals) {
                        stats.signals_generated++;

                        if (verbose) {
                            std::cout << "[SIGNAL] " << signal.strategy_name
                                     << " " << side_to_string(signal.side)
                                     << " @ " << signal.target_price
                                     << " edge=" << signal.expected_edge << "c"
                                     << " reason: " << signal.reason << "\n";
                        }

                        // Simulate trade execution
                        if (signal.expected_edge > config.min_edge_cents) {
                            stats.trades_simulated++;

                            // For S2, assume both sides fill
                            if (strategy_name == "s2") {
                                double edge_realized = signal.expected_edge * 0.8;  // 80% edge capture
                                double fee = 0.02;  // 2% fee
                                double pnl = (edge_realized / 100.0) - fee;
                                stats.total_pnl += pnl;
                                stats.total_fees += fee;
                            } else {
                                // S1 single-side: 50% win rate assumption
                                double win_rate = 0.55;
                                double outcome = (rand() % 100 < win_rate * 100) ? 1.0 : -1.0;
                                double pnl = outcome * signal.target_price * 0.1;  // 10% position size
                                stats.total_pnl += pnl;
                            }

                            // Track drawdown
                            if (stats.total_pnl > stats.peak_pnl) {
                                stats.peak_pnl = stats.total_pnl;
                            }
                            double drawdown = stats.peak_pnl - stats.total_pnl;
                            if (drawdown > stats.max_drawdown) {
                                stats.max_drawdown = drawdown;
                            }
                        }
                    }
                }
            }

            stats.messages_processed++;

            if (stats.messages_processed % 10000 == 0) {
                spdlog::info("Processed {} messages, {} signals, PnL: ${:.2f}",
                           stats.messages_processed, stats.signals_generated, stats.total_pnl);
            }

        } catch (const std::exception& e) {
            if (verbose) {
                spdlog::debug("Failed to parse line: {}", e.what());
            }
        }
    }

    // Print results
    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════\n";
    std::cout << "                    REPLAY RESULTS                       \n";
    std::cout << "════════════════════════════════════════════════════════\n";
    std::cout << "Strategy:           " << strategy_name << "\n";
    std::cout << "Messages processed: " << stats.messages_processed << "\n";
    std::cout << "Signals generated:  " << stats.signals_generated << "\n";
    std::cout << "Trades simulated:   " << stats.trades_simulated << "\n";
    std::cout << "────────────────────────────────────────────────────────\n";
    std::cout << "Total PnL:          $" << std::fixed << std::setprecision(2) << stats.total_pnl << "\n";
    std::cout << "Total fees:         $" << stats.total_fees << "\n";
    std::cout << "Net PnL:            $" << (stats.total_pnl - stats.total_fees) << "\n";
    std::cout << "Max drawdown:       $" << stats.max_drawdown << "\n";
    std::cout << "────────────────────────────────────────────────────────\n";

    if (stats.trades_simulated > 0) {
        std::cout << "Avg PnL/trade:      $" << (stats.total_pnl / stats.trades_simulated) << "\n";
        std::cout << "Win rate (est):     " << (stats.total_pnl > 0 ? "Positive" : "Negative") << "\n";
    }

    std::cout << "════════════════════════════════════════════════════════\n";
}

int main(int argc, char* argv[]) {
    CLI::App app{"DailyArb Replay Tool - Backtest strategies against recorded data"};

    std::string input_file;
    std::string strategy = "s2";
    std::string config_path = "configs/bot.json";
    bool verbose = false;

    app.add_option("-i,--input", input_file, "Input file with recorded market data")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("-s,--strategy", strategy, "Strategy to test (s1, s2)")
        ->default_val("s2");
    app.add_option("-c,--config", config_path, "Path to configuration file");
    app.add_flag("-v,--verbose", verbose, "Verbose output");

    CLI11_PARSE(app, argc, argv);

    // Load config
    Config config;
    try {
        if (std::filesystem::exists(config_path)) {
            config = Config::load(config_path);
        }
    } catch (...) {
        spdlog::warn("Using default configuration");
    }

    // Run replay
    run_replay(input_file, strategy, config.strategy, verbose);

    return 0;
}
