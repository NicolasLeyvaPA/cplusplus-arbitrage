#include <iostream>
#include <csignal>
#include <atomic>
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "common/types.hpp"
#include "config/config.hpp"
#include "market_data/binance_client.hpp"
#include "market_data/polymarket_client.hpp"
#include "strategy/strategy_base.hpp"
#include "risk/risk_manager.hpp"
#include "execution/execution_engine.hpp"
#include "position/position_manager.hpp"
#include "ui/terminal_ui.hpp"
#include "persistence/trade_ledger.hpp"
#include "utils/metrics.hpp"

using namespace arb;

// Global shutdown flag
std::atomic<bool> g_shutdown{false};

// Session end time (0 = no limit)
std::chrono::steady_clock::time_point g_session_end_time{};
bool g_has_session_limit{false};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        spdlog::info("Shutdown signal received");
        g_shutdown = true;
    }
}

// Parse duration string like "1.5h", "30m", "90s", "2h30m", "1h 30m"
// Returns duration in seconds, or 0 if invalid/empty
int64_t parse_duration_string(const std::string& input) {
    if (input.empty()) return 0;

    std::string s = input;
    // Remove spaces
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());

    // Convert to lowercase
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);

    int64_t total_seconds = 0;
    std::string number_buf;

    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (std::isdigit(c) || c == '.') {
            number_buf += c;
        } else if (c == 'h' || c == 'm' || c == 's') {
            if (number_buf.empty()) continue;

            double value = std::stod(number_buf);
            if (c == 'h') {
                total_seconds += static_cast<int64_t>(value * 3600);
            } else if (c == 'm') {
                total_seconds += static_cast<int64_t>(value * 60);
            } else if (c == 's') {
                total_seconds += static_cast<int64_t>(value);
            }
            number_buf.clear();
        }
    }

    // If only a number was provided (no unit), assume minutes
    if (!number_buf.empty() && total_seconds == 0) {
        double value = std::stod(number_buf);
        total_seconds = static_cast<int64_t>(value * 60);  // Default to minutes
    }

    return total_seconds;
}

// Format duration for display
std::string format_duration_display(int64_t seconds) {
    if (seconds <= 0) return "unlimited";

    int64_t hours = seconds / 3600;
    int64_t minutes = (seconds % 3600) / 60;
    int64_t secs = seconds % 60;

    std::ostringstream ss;
    if (hours > 0) {
        ss << hours << "h";
        if (minutes > 0) ss << " " << minutes << "m";
    } else if (minutes > 0) {
        ss << minutes << "m";
        if (secs > 0) ss << " " << secs << "s";
    } else {
        ss << secs << "s";
    }
    return ss.str();
}

// Prompt user for session duration
int64_t prompt_session_duration() {
    std::cout << "\n┌─────────────────────────────────────────────────────────────────────────────┐\n";
    std::cout << "│ SESSION DURATION                                                            │\n";
    std::cout << "├─────────────────────────────────────────────────────────────────────────────┤\n";
    std::cout << "│ How long do you want this session to run?                                  │\n";
    std::cout << "│                                                                             │\n";
    std::cout << "│ Examples:                                                                   │\n";
    std::cout << "│   1.5h     → 1 hour 30 minutes                                             │\n";
    std::cout << "│   30m      → 30 minutes                                                    │\n";
    std::cout << "│   2h30m    → 2 hours 30 minutes                                            │\n";
    std::cout << "│   90s      → 90 seconds                                                    │\n";
    std::cout << "│   (empty)  → Run until Ctrl+C                                              │\n";
    std::cout << "└─────────────────────────────────────────────────────────────────────────────┘\n";
    std::cout << "Enter duration: ";

    std::string input;
    std::getline(std::cin, input);

    int64_t seconds = parse_duration_string(input);
    if (seconds > 0) {
        std::cout << "Session will run for " << format_duration_display(seconds) << "\n\n";
    } else {
        std::cout << "Session will run until manually stopped (Ctrl+C)\n\n";
    }

    return seconds;
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
            config.log_dir + "/dailyarb.log",
            config.max_log_file_size_mb * 1024 * 1024,
            config.max_log_files
        );
        if (config.json_format) {
            file_sink->set_pattern(R"({"time":"%Y-%m-%dT%H:%M:%S.%e","level":"%l","msg":"%v"})");
        }
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("dailyarb", sinks.begin(), sinks.end());

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
}

void print_startup_banner(const Config& config) {
    std::cout << R"(
╔═══════════════════════════════════════════════════════════════════════════════╗
║                                                                               ║
║     ██████╗  █████╗ ██╗██╗  ██╗   ██╗ █████╗ ██████╗ ██████╗                  ║
║     ██╔══██╗██╔══██╗██║██║  ╚██╗ ██╔╝██╔══██╗██╔══██╗██╔══██╗                 ║
║     ██║  ██║███████║██║██║   ╚████╔╝ ███████║██████╔╝██████╔╝                 ║
║     ██║  ██║██╔══██║██║██║    ╚██╔╝  ██╔══██║██╔══██╗██╔══██╗                 ║
║     ██████╔╝██║  ██║██║███████╗██║   ██║  ██║██║  ██║██████╔╝                 ║
║     ╚═════╝ ╚═╝  ╚═╝╚═╝╚══════╝╚═╝   ╚═╝  ╚═╝╚═╝  ╚═╝╚═════╝                 ║
║                                                                               ║
║              Low-Latency Binary Outcome Arbitrage Bot                         ║
║                      ** RESEARCH TOOL ONLY **                                 ║
╚═══════════════════════════════════════════════════════════════════════════════╝
)" << std::endl;

    // CRITICAL DISCLAIMER
    std::cout << "╔═══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  CRITICAL: THIS SYSTEM CANNOT TRADE REAL MONEY PROFITABLY                     ║\n";
    std::cout << "╠═══════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  - Paper trading uses ADVERSARIAL assumptions, still not predictive           ║\n";
    std::cout << "║  - Live mode sends paired orders SEQUENTIALLY (non-atomic, creates exposure)  ║\n";
    std::cout << "║  - Competition is faster than you                                             ║\n";
    std::cout << "║  - Gas fees exceed edge on small trades                                       ║\n";
    std::cout << "║                                                                               ║\n";
    std::cout << "║  See TRUTH.md and DISCLAIMER.md for forensic analysis.                        ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════════════════╝\n\n";

    std::cout << "┌─────────────────────────────────────────────────────────────────────────────┐\n";
    std::cout << "│ RISK & REALITY CHECK                                                        │\n";
    std::cout << "├─────────────────────────────────────────────────────────────────────────────┤\n";
    std::cout << "│ This is NOT financial advice. Trading involves substantial risk of loss.   │\n";
    std::cout << "│                                                                             │\n";
    std::cout << "│ Key considerations:                                                         │\n";
    std::cout << "│ • Fees: Polymarket charges ~2% on winnings                                  │\n";
    std::cout << "│ • Slippage: Fast-moving markets may have execution slippage                │\n";
    std::cout << "│ • Min sizes: Check minimum order sizes before trading                       │\n";
    std::cout << "│ • Fills: IOC/FOK orders may not fill completely                            │\n";
    std::cout << "│ • Network: Latency and disconnections can cause missed opportunities       │\n";
    std::cout << "│                                                                             │\n";
    std::cout << "│ Starting capital: $" << std::fixed << std::setprecision(2) << config.starting_balance_usdc << " USDC\n";
    std::cout << "│ Max per trade:    $" << config.risk.max_notional_per_trade << "\n";
    std::cout << "│ Max daily loss:   $" << config.risk.max_daily_loss << "\n";
    std::cout << "└─────────────────────────────────────────────────────────────────────────────┘\n\n";

    if (config.mode == TradingMode::LIVE) {
        std::cout << "╔═══════════════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  ⚠️  WARNING: LIVE TRADING MODE ENABLED                                       ║\n";
        std::cout << "║  Real orders will be placed. Real money at risk.                              ║\n";
        std::cout << "╚═══════════════════════════════════════════════════════════════════════════════╝\n\n";
    } else if (config.mode == TradingMode::PAPER) {
        std::cout << "[PAPER MODE] Simulated execution against local model.\n\n";
    } else {
        std::cout << "[DRY-RUN MODE] Signals computed, no orders placed.\n\n";
    }
}

int main(int argc, char* argv[]) {
    // CLI parsing
    CLI::App app{"DailyArb - Low-Latency Binary Outcome Arbitrage Bot"};

    std::string config_path = "configs/bot.json";
    bool dry_run = false;
    bool paper_mode = false;
    bool live_mode = false;
    bool show_version = false;
    bool list_markets = false;

    app.add_option("-c,--config", config_path, "Path to configuration file")
        ->check(CLI::ExistingFile);
    app.add_flag("--dry-run", dry_run, "Dry-run mode: compute signals without placing orders");
    app.add_flag("--paper", paper_mode, "Paper trading mode: simulated execution");
    app.add_flag("--live", live_mode, "Live trading mode (requires explicit confirmation)");
    app.add_flag("-v,--version", show_version, "Show version information");
    app.add_flag("--list-markets", list_markets, "List all available Polymarket markets and exit");

    CLI11_PARSE(app, argc, argv);

    if (show_version) {
        std::cout << "DailyArb v1.0.0\n";
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

        // Require explicit confirmation for live mode
        std::cout << "⚠️  LIVE TRADING MODE REQUESTED ⚠️\n";
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

    // Print startup banner
    print_startup_banner(config);

    // Prompt for session duration
    int64_t session_duration_secs = prompt_session_duration();
    if (session_duration_secs > 0) {
        g_has_session_limit = true;
        g_session_end_time = std::chrono::steady_clock::now() +
                             std::chrono::seconds(session_duration_secs);
        spdlog::info("Session will run for {} ({} seconds)",
                     format_duration_display(session_duration_secs), session_duration_secs);
    } else {
        spdlog::info("Session will run until manually stopped (Ctrl+C)");
    }

    // Signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize components
    spdlog::info("Initializing DailyArb...");

    // Risk manager
    auto risk_manager = std::make_shared<RiskManager>(config.risk, config.starting_balance_usdc);

    // Position manager
    auto position_manager = std::make_shared<PositionManager>();

    // Market data clients
    auto binance_client = std::make_shared<BinanceClient>(config.connection);
    auto polymarket_client = std::make_shared<PolymarketClient>(config.connection);

    // Load API credentials from environment
    std::string poly_key = Config::get_env("POLYMARKET_API_KEY");
    std::string poly_secret = Config::get_env("POLYMARKET_API_SECRET");
    std::string poly_passphrase = Config::get_env("POLYMARKET_API_PASSPHRASE");

    if (!poly_key.empty() && !poly_secret.empty()) {
        polymarket_client->set_api_credentials(poly_key, poly_secret, poly_passphrase);
        spdlog::info("Polymarket API credentials loaded from environment");
    } else if (config.mode == TradingMode::LIVE) {
        spdlog::error("LIVE mode requires API credentials. Set POLYMARKET_API_KEY and POLYMARKET_API_SECRET");
        return 1;
    }

    // Execution engine
    auto execution_engine = std::make_shared<ExecutionEngine>(
        config.mode, risk_manager, polymarket_client
    );

    // Trade ledger
    auto trade_ledger = std::make_shared<TradeLedger>(config.trade_ledger_path);

    // Strategies
    std::vector<std::unique_ptr<StrategyBase>> strategies;
    if (config.strategy.enable_s2) {
        strategies.push_back(std::make_unique<UnderpricingStrategy>(config.strategy));
    }
    if (config.strategy.enable_s1) {
        strategies.push_back(std::make_unique<StaleOddsStrategy>(config.strategy));
    }
    if (config.strategy.enable_s3) {
        strategies.push_back(std::make_unique<MarketMakingStrategy>(config.strategy));
    }

    // Terminal UI
    auto ui = std::make_shared<TerminalUI>(
        config.mode, binance_client, polymarket_client,
        position_manager, risk_manager, execution_engine
    );

    // Wire up callbacks
    execution_engine->set_fill_callback([&](const Fill& fill) {
        position_manager->record_fill(fill);
        trade_ledger->record_fill(fill);
        ui->log_trade(fill);
        METRIC_COUNTER("fills").increment();
    });

    execution_engine->set_order_callback([&](const Order& order) {
        trade_ledger->record_order(order);
        ui->log_order(order.client_order_id, order_state_to_string(order.state));
    });

    binance_client->set_status_callback([&](ConnectionStatus status) {
        if (status == ConnectionStatus::CONNECTED) {
            ui->log_info("Binance connected");
        } else if (status == ConnectionStatus::ERROR) {
            ui->log_error("Binance connection error");
            risk_manager->record_connectivity_issue();
        }
    });

    polymarket_client->set_status_callback([&](ConnectionStatus status) {
        if (status == ConnectionStatus::CONNECTED) {
            ui->log_info("Polymarket connected");
        } else if (status == ConnectionStatus::ERROR) {
            ui->log_error("Polymarket connection error");
            risk_manager->record_connectivity_issue();
        }
    });

    // Start connections
    spdlog::info("Connecting to data sources...");
    binance_client->connect();
    polymarket_client->connect();

    // Wait for initial connection
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Handle --list-markets flag
    if (list_markets) {
        spdlog::info("Fetching all available Polymarket markets...");
        auto all_markets = polymarket_client->fetch_markets();

        std::cout << "\n╔═══════════════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  Available Polymarket Markets (" << all_markets.size() << " total)                                  ║\n";
        std::cout << "╚═══════════════════════════════════════════════════════════════════════════════╝\n\n";

        for (const auto& m : all_markets) {
            std::cout << "• " << m.question << "\n";
            std::cout << "  Slug: " << m.slug << "\n";
            std::cout << "  ID: " << m.condition_id.substr(0, 20) << "...\n";
            std::cout << "  YES token: " << m.yes_outcome.token_id.substr(0, 16) << "...\n";
            std::cout << "  NO token:  " << m.no_outcome.token_id.substr(0, 16) << "...\n\n";
        }

        std::cout << "To trade specific markets, set 'market_pattern' in configs/bot.json\n";
        std::cout << "Example patterns:\n";
        std::cout << "  \"\"                  - Trade ALL markets (S2 underpricing on any binary)\n";
        std::cout << "  \"bitcoin|btc|eth\"   - Trade crypto-related markets\n";
        std::cout << "  \"president|election\" - Trade political markets\n";

        binance_client->disconnect();
        polymarket_client->disconnect();
        return 0;
    }

    // Fetch and subscribe to markets using config pattern
    spdlog::info("Fetching markets with pattern: '{}'", config.market_pattern.empty() ? "(all)" : config.market_pattern);
    auto markets = polymarket_client->fetch_filtered_markets(config.market_pattern);

    if (markets.empty()) {
        spdlog::warn("No markets found matching pattern '{}'. Use --list-markets to see available options.",
                     config.market_pattern);
        spdlog::warn("Tip: Set market_pattern to \"\" in config to trade all markets with S2 underpricing.");
    } else {
        spdlog::info("Found {} markets to monitor", markets.size());
        for (const auto& market : markets) {
            polymarket_client->subscribe_market(market.yes_outcome.token_id);
            polymarket_client->subscribe_market(market.no_outcome.token_id);

            // Set first market as active in UI
            if (ui) {
                ui->set_active_market(market.condition_id);
            }

            spdlog::info("Subscribed to: {}", market.question);
        }
    }

    // Start UI
    ui->start();

    spdlog::info("DailyArb started. Mode: {}", mode_to_string(config.mode));

    // Main trading loop
    while (!g_shutdown.load()) {
        // Check session time limit
        if (g_has_session_limit) {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                g_session_end_time - std::chrono::steady_clock::now()
            );
            if (remaining.count() <= 0) {
                spdlog::info("Session time limit reached. Shutting down...");
                g_shutdown = true;
                break;
            }
            // Log remaining time periodically (every 60 seconds)
            static auto last_log_time = std::chrono::steady_clock::now();
            auto since_last_log = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - last_log_time
            );
            if (since_last_log.count() >= 60) {
                spdlog::info("Session time remaining: {}", format_duration_display(remaining.count()));
                last_log_time = std::chrono::steady_clock::now();
            }
        }

        // Check if we should continue trading
        if (risk_manager->should_halt_trading()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // Get current BTC price
        BtcPrice btc_price = binance_client->current_price();
        Timestamp now_time = now();

        // Evaluate strategies for each market
        for (const auto& market : markets) {
            auto* book = polymarket_client->get_market_book(market.condition_id);
            if (!book || !book->has_liquidity()) {
                continue;
            }

            // Update mark prices for position manager
            if (auto yes_ask = book->yes_book().best_ask()) {
                position_manager->mark_to_market(market.yes_outcome.token_id, yes_ask->price);
            }
            if (auto no_ask = book->no_book().best_ask()) {
                position_manager->mark_to_market(market.no_outcome.token_id, no_ask->price);
            }

            // Evaluate each strategy
            for (auto& strategy : strategies) {
                if (!strategy->is_enabled()) continue;

                auto signals = strategy->evaluate(*book, btc_price, now_time);

                for (const auto& signal : signals) {
                    ui->log_signal(signal);
                    trade_ledger->record_signal(signal);
                    METRIC_COUNTER("signals").increment();

                    // For S2 (underpricing), we need paired execution
                    if (signal.strategy_name == "S2_Underpricing" && signals.size() >= 2) {
                        // Find the matching pair
                        for (size_t i = 0; i < signals.size(); i++) {
                            for (size_t j = i + 1; j < signals.size(); j++) {
                                if (signals[i].market_id == signals[j].market_id &&
                                    signals[i].token_id != signals[j].token_id) {
                                    auto result = execution_engine->submit_paired_order(signals[i], signals[j]);
                                    if (result.accepted) {
                                        spdlog::info("Paired order submitted: {}", result.order_id);
                                    }
                                }
                            }
                        }
                        break;  // Only submit one pair per evaluation
                    } else {
                        // Single-side order for S1
                        auto result = execution_engine->submit_order(signal);
                        if (result.accepted) {
                            strategy->record_signal_acted();
                        }
                    }
                }
            }
        }

        // Update metrics
        METRIC_GAUGE("balance").set(risk_manager->available_balance());
        METRIC_GAUGE("daily_pnl").set(risk_manager->daily_pnl());
        METRIC_GAUGE("exposure").set(risk_manager->current_exposure());

        // Small sleep to control loop rate
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Shutdown
    spdlog::info("Shutting down...");

    // Cancel any open orders
    execution_engine->cancel_all();

    // Stop UI
    ui->stop();

    // Disconnect
    binance_client->disconnect();
    polymarket_client->disconnect();

    // Save final state
    trade_ledger->flush();

    // Print final stats
    spdlog::info("Final PnL: ${:.2f}", position_manager->total_pnl());
    spdlog::info("Total trades: {}", execution_engine->orders_filled());
    spdlog::info("Total fees: ${:.2f}", position_manager->total_fees());

    std::cout << "\nMetrics:\n" << MetricsRegistry::instance().to_json() << "\n";

    spdlog::info("DailyArb shutdown complete.");
    return 0;
}
