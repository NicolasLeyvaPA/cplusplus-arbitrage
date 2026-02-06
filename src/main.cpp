#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "common/types.hpp"
#include "config/config.hpp"
#include "exchange/binance_spot.hpp"
#include "exchange/binance_futures.hpp"
#include "strategy/funding_monitor.hpp"
#include "strategy/basis_calculator.hpp"
#include "strategy/delta_neutral_engine.hpp"
#include "execution/execution_engine.hpp"
#include "execution/hedge_manager.hpp"
#include "position/position_manager.hpp"
#include "risk/risk_manager.hpp"
#include "ui/terminal_ui.hpp"

using namespace arb;

std::atomic<bool> g_shutdown{false};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        spdlog::info("Shutdown signal received");
        g_shutdown = true;
    }
}

void setup_logging(const LoggingConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;

    if (config.log_to_console) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        sinks.push_back(console_sink);
    }

    if (config.log_to_file) {
        std::filesystem::create_directories(config.log_dir);
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.log_dir + "/delta_neutral.log",
            50 * 1024 * 1024,  // 50MB
            5
        );
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("delta_neutral", sinks.begin(), sinks.end());

    if (config.log_level == "debug") {
        logger->set_level(spdlog::level::debug);
    } else if (config.log_level == "warn") {
        logger->set_level(spdlog::level::warn);
    } else if (config.log_level == "error") {
        logger->set_level(spdlog::level::err);
    } else {
        logger->set_level(spdlog::level::info);
    }

    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(3));
}

int main(int argc, char* argv[]) {
    // CLI parsing
    CLI::App app{"Delta-Neutral Funding Rate Arbitrage Bot"};

    std::string config_path = "configs/bot.json";
    bool dry_run = false;
    bool paper_mode = false;
    bool live_mode = false;
    bool show_version = false;

    app.add_option("-c,--config", config_path, "Path to configuration file");
    app.add_flag("--dry-run", dry_run, "Dry-run mode: compute signals without placing orders");
    app.add_flag("--paper", paper_mode, "Paper trading mode: simulated execution");
    app.add_flag("--live", live_mode, "Live trading mode (requires explicit confirmation)");
    app.add_flag("-v,--version", show_version, "Show version information");

    CLI11_PARSE(app, argc, argv);

    if (show_version) {
        std::cout << "Delta-Neutral Funding Arbitrage Bot v1.0.0\n";
        std::cout << "Built with C++20\n";
        return 0;
    }

    // Load config
    Config config;
    try {
        if (std::filesystem::exists(config_path)) {
            config = Config::load(config_path);
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to load config: " << e.what() << "\n";
        std::cerr << "Using default configuration.\n";
    }

    // Override mode from command line
    if (dry_run) {
        config.mode = TradingMode::DRY_RUN;
    } else if (paper_mode) {
        config.mode = TradingMode::PAPER;
    } else if (live_mode) {
        config.mode = TradingMode::LIVE;

        std::cout << "WARNING: LIVE TRADING MODE REQUESTED\n";
        std::cout << "This will place REAL orders with REAL money.\n";
        std::cout << "Type 'CONFIRM' to proceed: ";
        std::string confirmation;
        std::getline(std::cin, confirmation);
        if (confirmation != "CONFIRM") {
            std::cout << "Live trading not confirmed. Exiting.\n";
            return 1;
        }
    }

    // Setup logging
    setup_logging(config.logging);

    // Startup banner
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  DELTA-NEUTRAL FUNDING ARBITRAGE BOT\n";
    std::cout << "========================================\n";
    std::cout << "  Mode:     " << arb::to_string(config.mode) << "\n";
    std::cout << "  Symbols:  ";
    for (size_t i = 0; i < config.strategy.symbols.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << config.strategy.symbols[i];
    }
    std::cout << "\n";
    std::cout << "  Capital:  $" << config.capital.starting_balance_usd << "\n";
    std::cout << "  Max Exp:  $" << config.risk.max_total_exposure_usd << "\n";
    std::cout << "  Testnet:  " << (config.exchange.testnet ? "YES" : "NO") << "\n";
    std::cout << "========================================\n\n";

    // Validate config
    try {
        config.validate();
    } catch (const std::exception& e) {
        spdlog::error("Config validation failed: {}", e.what());
        return 1;
    }

    // Signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize exchange clients
    spdlog::info("Initializing exchange clients...");
    BinanceSpot spot(config.exchange);
    BinanceFutures futures(config.exchange);

    // Connect
    spdlog::info("Connecting to Binance{}...", config.exchange.testnet ? " Testnet" : "");
    if (!spot.connect()) {
        spdlog::error("Failed to connect to Binance Spot");
        return 1;
    }
    if (!futures.connect()) {
        spdlog::error("Failed to connect to Binance Futures");
        return 1;
    }
    spdlog::info("Exchange connections established");

    // Configure futures leverage and margin type
    for (const auto& symbol : config.strategy.symbols) {
        futures.set_leverage(symbol, config.risk.futures_leverage);
        futures.set_margin_type(symbol, config.risk.margin_type);
        spdlog::info("Configured {}: leverage={}, margin={}", symbol,
                     config.risk.futures_leverage, config.risk.margin_type);
    }

    // Initialize components
    FundingMonitor funding_monitor(futures, config.strategy);
    BasisCalculator basis_calculator;
    PositionManager position_manager;
    RiskManager risk_manager(config.risk);
    ExecutionEngine execution(spot, futures, config.mode);
    HedgeManager hedge_manager(execution, config.strategy.rebalance_threshold_pct);
    DeltaNeutralEngine engine(funding_monitor, basis_calculator,
                              config.strategy, config.risk);
    TerminalUI ui;

    // Start funding monitor background polling
    funding_monitor.start();
    spdlog::info("Funding rate monitor started");

    // Initial funding rate fetch
    spdlog::info("Fetching initial funding rates...");
    funding_monitor.refresh();
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Timing control
    auto last_price_update = std::chrono::steady_clock::now();
    auto last_strategy_eval = std::chrono::steady_clock::now();
    auto last_hedge_check = std::chrono::steady_clock::now();
    auto last_pnl_update = std::chrono::steady_clock::now();
    auto last_ui_render = std::chrono::steady_clock::now();

    std::vector<std::string> event_log;

    auto log_event = [&](const std::string& msg) {
        event_log.push_back(msg);
        if (event_log.size() > 50) {
            event_log.erase(event_log.begin());
        }
        ui.log_event(msg);
        spdlog::info("{}", msg);
    };

    log_event("Bot started in " + arb::to_string(config.mode) + " mode");

    spdlog::info("Entering main loop...");

    // =========================================================================
    // MAIN LOOP
    // =========================================================================
    while (!g_shutdown.load() && !ui.should_quit()) {
        auto loop_now = std::chrono::steady_clock::now();

        // Kill switch check
        if (risk_manager.is_kill_switch_active()) {
            auto positions = position_manager.get_all_positions();
            for (const auto& [symbol, pos] : positions) {
                if (pos.is_open()) {
                    hedge_manager.emergency_close(symbol, pos);
                    position_manager.close_position(symbol);
                    log_event("KILL SWITCH: Closed " + symbol);
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // -------------------------------------------------------------------
        // UPDATE PRICES (every price_update_interval_ms)
        // -------------------------------------------------------------------
        auto price_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            loop_now - last_price_update).count();
        if (price_elapsed >= config.polling.price_update_interval_ms) {
            last_price_update = loop_now;

            for (const auto& symbol : config.strategy.symbols) {
                try {
                    auto spot_ticker = spot.get_ticker(symbol);
                    auto futures_ticker = futures.get_ticker(symbol);

                    basis_calculator.update(symbol, spot_ticker.mid(), futures_ticker.mid());
                    position_manager.mark_to_market(symbol, spot_ticker.mid(), futures_ticker.mid());
                } catch (const std::exception& e) {
                    spdlog::warn("Price update failed for {}: {}", symbol, e.what());
                }
            }
        }

        // -------------------------------------------------------------------
        // STRATEGY EVALUATION (every strategy_eval_interval_s)
        // -------------------------------------------------------------------
        auto strategy_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            loop_now - last_strategy_eval).count();
        if (strategy_elapsed >= config.polling.strategy_eval_interval_s) {
            last_strategy_eval = loop_now;

            double available_capital = config.capital.starting_balance_usd *
                                       (config.capital.allocation_pct / 100.0);
            available_capital -= position_manager.total_exposure();

            auto positions = position_manager.get_all_positions();
            auto signals = engine.evaluate(positions, available_capital);

            for (const auto& signal : signals) {
                spdlog::info("Signal: {} {} size={:.6f} rate={:.4f}% yield={:.1f}%",
                             arb::to_string(signal.type), signal.symbol,
                             signal.target_size, signal.expected_funding_rate * 100,
                             signal.expected_annual_yield);

                if (signal.type == SignalType::OPEN) {
                    auto spot_ticker = spot.get_ticker(signal.symbol);
                    Notional exposure = signal.target_size * spot_ticker.mid();
                    auto risk_check = risk_manager.check_new_position(
                        signal.symbol, exposure, position_manager);
                    if (!risk_check.allowed) {
                        log_event("Risk blocked OPEN " + signal.symbol + ": " + risk_check.reason);
                        continue;
                    }

                    auto funding_check = risk_manager.check_funding_rate(signal.expected_funding_rate);
                    if (!funding_check.allowed) {
                        log_event("Risk blocked " + signal.symbol + ": " + funding_check.reason);
                        continue;
                    }

                    auto futures_ticker = futures.get_ticker(signal.symbol);
                    auto result = execution.open_delta_neutral(
                        signal.symbol, signal.target_size,
                        spot_ticker.ask, futures_ticker.bid);

                    if (result.success) {
                        position_manager.update_spot_fill(
                            signal.symbol, result.spot_response.executed_qty,
                            result.spot_response.price, 0);
                        position_manager.update_futures_fill(
                            signal.symbol, -result.futures_response.executed_qty,
                            result.futures_response.price, 0);
                        log_event("OPENED " + signal.symbol + " size=" +
                                  std::to_string(signal.target_size));
                    } else {
                        log_event("OPEN FAILED " + signal.symbol + ": " + result.error);
                    }
                }
                else if (signal.type == SignalType::CLOSE) {
                    auto pos_opt = position_manager.get_position(signal.symbol);
                    if (pos_opt) {
                        auto result = execution.close_delta_neutral(signal.symbol, *pos_opt);
                        if (result.success) {
                            log_event("CLOSED " + signal.symbol + " PnL=" +
                                      std::to_string(pos_opt->total_pnl()));
                            position_manager.close_position(signal.symbol);
                        } else {
                            log_event("CLOSE FAILED " + signal.symbol + ": " + result.error);
                        }
                    }
                }
            }
        }

        // -------------------------------------------------------------------
        // HEDGE CHECK (every hedge_check_interval_s)
        // -------------------------------------------------------------------
        auto hedge_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            loop_now - last_hedge_check).count();
        if (hedge_elapsed >= config.polling.hedge_check_interval_s) {
            last_hedge_check = loop_now;

            auto positions = position_manager.get_all_positions();
            for (const auto& [symbol, pos] : positions) {
                if (!pos.is_open()) continue;
                try {
                    auto spot_ticker = spot.get_ticker(symbol);
                    auto futures_ticker = futures.get_ticker(symbol);
                    auto rebalance = hedge_manager.check_and_rebalance(
                        symbol, pos, spot_ticker, futures_ticker);
                    if (rebalance.needed && rebalance.executed) {
                        log_event("REBALANCED " + symbol + ": " + rebalance.reason);
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Hedge check failed for {}: {}", symbol, e.what());
                }
            }
        }

        // -------------------------------------------------------------------
        // P&L UPDATE (every pnl_snapshot_interval_s)
        // -------------------------------------------------------------------
        auto pnl_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            loop_now - last_pnl_update).count();
        if (pnl_elapsed >= config.polling.pnl_snapshot_interval_s) {
            last_pnl_update = loop_now;

            double total_pnl = position_manager.total_pnl();
            risk_manager.record_daily_pnl(total_pnl);

            auto loss_check = risk_manager.check_daily_loss(total_pnl);
            if (!loss_check.allowed) {
                risk_manager.activate_kill_switch("Daily loss limit: " + loss_check.reason);
                log_event("KILL SWITCH: " + loss_check.reason);
            }
        }

        // -------------------------------------------------------------------
        // UI RENDER (every 1 second)
        // -------------------------------------------------------------------
        auto ui_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            loop_now - last_ui_render).count();
        if (ui_elapsed >= 1000) {
            last_ui_render = loop_now;

            ui.check_input();

            auto funding = funding_monitor.get_all_funding();
            auto basis = basis_calculator.get_all_basis();
            auto positions = position_manager.get_all_positions();
            double total_pnl = position_manager.total_pnl();
            double total_funding = position_manager.total_funding();

            ui.render(config.mode, funding, basis, positions,
                      total_pnl, 0.0, total_funding,
                      risk_manager.is_kill_switch_active(), event_log);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // =========================================================================
    // SHUTDOWN
    // =========================================================================
    spdlog::info("Shutting down...");

    funding_monitor.stop();

    auto final_positions = position_manager.get_all_positions();
    for (const auto& [symbol, pos] : final_positions) {
        if (pos.is_open()) {
            spdlog::info("Position still open at shutdown: {} PnL={:.4f}", symbol, pos.total_pnl());
        }
    }

    spot.disconnect();
    futures.disconnect();

    spdlog::info("=== Session Summary ===");
    spdlog::info("Total PnL:     ${:.4f}", position_manager.total_pnl());
    spdlog::info("Total Funding: ${:.4f}", position_manager.total_funding());
    spdlog::info("Total Exposure: ${:.2f}", position_manager.total_exposure());
    spdlog::info("Shutdown complete.");

    return 0;
}
