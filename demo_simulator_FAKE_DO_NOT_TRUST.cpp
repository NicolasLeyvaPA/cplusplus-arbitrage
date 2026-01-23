// DailyArb Demo Simulator - Self-contained arbitrage demonstration
// Simulates BTC binary outcome markets with occasional arbitrage opportunities

#include <iostream>
#include <iomanip>
#include <random>
#include <chrono>
#include <thread>
#include <cmath>
#include <vector>
#include <string>
#include <sstream>

// Terminal colors
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"

class ArbitrageSimulator {
public:
    // Market state
    double btc_price = 97500.0;
    double btc_24h_high = 98200.0;
    double btc_24h_low = 96800.0;

    // Binary market: "Will BTC be above $97,500 at market close?"
    double yes_best_bid = 0.48;
    double yes_best_ask = 0.52;
    double no_best_bid = 0.46;
    double no_best_ask = 0.50;

    // Trading state
    double balance = 50.00;          // Starting balance (demo money)
    double total_pnl = 0.0;
    double realized_pnl = 0.0;
    int total_trades = 0;
    int winning_trades = 0;
    int signals_detected = 0;
    double total_fees = 0.0;

    // Position tracking
    struct Position {
        std::string token;
        double quantity = 0;
        double avg_entry = 0;
        double cost_basis = 0;
    };
    Position yes_position;
    Position no_position;

    // Constants
    const double POLYMARKET_FEE = 0.02;  // 2% fee on winnings
    const double MIN_EDGE_CENTS = 0.02;  // Minimum 2 cent edge after fees
    const double MAX_TRADE_SIZE = 1.50;  // Max $1.50 per trade (3% of $50)

    std::mt19937 rng{std::random_device{}()};

    void print_banner() {
        std::cout << CYAN << R"(
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
║                           ~ DEMO MODE ~                                       ║
╚═══════════════════════════════════════════════════════════════════════════════╝
)" << RESET << std::endl;

        std::cout << YELLOW << "┌─────────────────────────────────────────────────────────────────────────────┐\n";
        std::cout << "│ PAPER TRADING MODE - Simulated execution with demo money                     │\n";
        std::cout << "├─────────────────────────────────────────────────────────────────────────────┤\n";
        std::cout << "│ Starting capital: $50.00 USDC                                               │\n";
        std::cout << "│ Max per trade:    $1.50                                                     │\n";
        std::cout << "│ Strategy:         S2 - Two-Outcome Underpricing Arbitrage                  │\n";
        std::cout << "│                                                                             │\n";
        std::cout << "│ How it works: When YES_ask + NO_ask < $0.98 (after 2% fees), buy both      │\n";
        std::cout << "│ sides. At expiry, one side pays $1.00, guaranteed profit!                  │\n";
        std::cout << "└─────────────────────────────────────────────────────────────────────────────┘" << RESET << "\n\n";
    }

    void update_btc_price() {
        // Random walk with stronger mean reversion to keep BTC near target
        std::normal_distribution<> noise(0.0, 0.0003);  // Lower volatility
        double change_pct = noise(rng);

        // Stronger mean reversion towards 97500 to keep price stable
        double mean_reversion = (97500.0 - btc_price) / 97500.0 * 0.01;
        change_pct += mean_reversion;

        btc_price *= (1.0 + change_pct);
        btc_24h_high = std::max(btc_24h_high, btc_price);
        btc_24h_low = std::min(btc_24h_low, btc_price);
    }

    void update_order_books() {
        // Update fair value based on BTC price (simple model)
        double fair_yes = 0.5 + (btc_price - 97500.0) / 2000.0;  // More likely YES if BTC higher
        fair_yes = std::max(0.1, std::min(0.9, fair_yes));
        double fair_no = 1.0 - fair_yes;

        // Normal spread
        std::uniform_real_distribution<> spread_dist(0.02, 0.05);
        double spread = spread_dist(rng);

        // Occasionally create underpricing opportunity (about 25% of the time)
        std::uniform_real_distribution<> opportunity_dist(0.0, 1.0);
        bool create_opportunity = opportunity_dist(rng) < 0.25;

        if (create_opportunity) {
            // Make the combined asks less than $0.98 (creating arb opportunity)
            std::uniform_real_distribution<> edge_dist(0.03, 0.06);
            double edge = edge_dist(rng);  // 3-6 cent edge

            yes_best_ask = fair_yes - edge/2;
            no_best_ask = fair_no - edge/2;
        } else {
            // Normal pricing (no opportunity)
            yes_best_ask = fair_yes + spread/2;
            no_best_ask = fair_no + spread/2;
        }

        // Set bids slightly below fair value
        yes_best_bid = fair_yes - spread/2;
        no_best_bid = fair_no - spread/2;

        // Ensure valid prices
        yes_best_ask = std::max(0.01, std::min(0.99, yes_best_ask));
        yes_best_bid = std::max(0.01, std::min(yes_best_ask - 0.01, yes_best_bid));
        no_best_ask = std::max(0.01, std::min(0.99, no_best_ask));
        no_best_bid = std::max(0.01, std::min(no_best_ask - 0.01, no_best_bid));
    }

    bool check_underpricing_opportunity(double& edge, double& trade_size) {
        // S2 Strategy: Check if sum of best asks < 1.0 - fees
        double sum_of_asks = yes_best_ask + no_best_ask;
        double threshold = 1.0 - POLYMARKET_FEE;  // 0.98 (need sum < this to profit)

        if (sum_of_asks < threshold) {
            edge = threshold - sum_of_asks;
            // Only trade if edge is meaningful (> 0.5 cents)
            if (edge < 0.005) return false;
            // Calculate trade size (limited by balance and max trade size)
            trade_size = std::min({MAX_TRADE_SIZE, balance / 2.0, balance - 1.0});
            trade_size = std::max(0.0, trade_size);
            return trade_size > 0.10;  // Minimum trade size
        }
        return false;
    }

    void execute_arbitrage_trade(double edge, double trade_size) {
        signals_detected++;

        // Calculate quantities
        double yes_qty = trade_size / yes_best_ask;
        double no_qty = trade_size / no_best_ask;
        double total_qty = std::min(yes_qty, no_qty);  // Match quantities

        double yes_cost = total_qty * yes_best_ask;
        double no_cost = total_qty * no_best_ask;
        double total_cost = yes_cost + no_cost;

        // Calculate expected profit
        // One side will pay out $1.00 per share, minus 2% fee
        double payout = total_qty * (1.0 - POLYMARKET_FEE);
        double expected_profit = payout - total_cost;

        if (expected_profit > 0 && total_cost <= balance) {
            // Execute trade
            balance -= total_cost;
            total_fees += total_cost * 0.001;  // Small trading fee

            // Record positions
            yes_position.quantity += total_qty;
            yes_position.cost_basis += yes_cost;
            yes_position.avg_entry = yes_position.cost_basis / yes_position.quantity;

            no_position.quantity += total_qty;
            no_position.cost_basis += no_cost;
            no_position.avg_entry = no_position.cost_basis / no_position.quantity;

            total_trades++;

            print_trade_execution(total_qty, yes_best_ask, no_best_ask, edge, expected_profit);
        }
    }

    void settle_positions() {
        // Simulate market resolution (randomly determine if BTC ended above target)
        if (yes_position.quantity > 0 && no_position.quantity > 0) {
            std::uniform_real_distribution<> dist(0.0, 1.0);

            // Settlement probability based on current BTC position
            double prob_yes = 0.5 + (btc_price - 97500.0) / 4000.0;
            prob_yes = std::max(0.2, std::min(0.8, prob_yes));

            bool yes_wins = dist(rng) < prob_yes;

            double winning_qty = yes_wins ? yes_position.quantity : no_position.quantity;
            double payout = winning_qty * (1.0 - POLYMARKET_FEE);  // $1 per share minus fee
            double total_cost = yes_position.cost_basis + no_position.cost_basis;
            double profit = payout - total_cost;

            balance += payout;
            realized_pnl += profit;
            total_pnl = realized_pnl;

            if (profit > 0) winning_trades++;

            print_settlement(yes_wins, profit, payout);

            // Reset positions
            yes_position = Position{};
            no_position = Position{};
        }
    }

    void print_trade_execution(double qty, double yes_price, double no_price, double edge, double expected_pnl) {
        std::cout << "\n" << GREEN << BOLD << "═══════════════════════════════════════════════════════════════" << RESET << "\n";
        std::cout << GREEN << BOLD << "  🎯 ARBITRAGE OPPORTUNITY DETECTED & EXECUTED!" << RESET << "\n";
        std::cout << GREEN << "═══════════════════════════════════════════════════════════════" << RESET << "\n";
        std::cout << "  Market: BTC Up/Down 15m Binary\n";
        std::cout << "  Strategy: S2 - Two-Outcome Underpricing\n";
        std::cout << "\n";
        std::cout << "  " << CYAN << "BUY YES" << RESET << " @ $" << std::fixed << std::setprecision(4) << yes_price;
        std::cout << "  x " << std::setprecision(2) << qty << " shares = $" << (qty * yes_price) << "\n";
        std::cout << "  " << MAGENTA << "BUY NO " << RESET << " @ $" << std::setprecision(4) << no_price;
        std::cout << "  x " << std::setprecision(2) << qty << " shares = $" << (qty * no_price) << "\n";
        std::cout << "\n";
        std::cout << "  Sum of asks: $" << std::setprecision(4) << (yes_price + no_price) << " < $0.98 threshold\n";
        std::cout << "  Edge found:  " << GREEN << std::setprecision(1) << (edge * 100) << " cents" << RESET << " per share\n";
        std::cout << "  Expected profit: " << GREEN << "$" << std::setprecision(4) << expected_pnl << RESET << "\n";
        std::cout << GREEN << "═══════════════════════════════════════════════════════════════" << RESET << "\n\n";
    }

    void print_settlement(bool yes_wins, double profit, double payout) {
        std::cout << "\n" << YELLOW << BOLD << "═══════════════════════════════════════════════════════════════" << RESET << "\n";
        std::cout << YELLOW << BOLD << "  📊 MARKET SETTLED - POSITION CLOSED" << RESET << "\n";
        std::cout << YELLOW << "═══════════════════════════════════════════════════════════════" << RESET << "\n";
        std::cout << "  Outcome: " << (yes_wins ? "YES (BTC above target)" : "NO (BTC below target)") << "\n";
        std::cout << "  Payout received: $" << std::fixed << std::setprecision(4) << payout << "\n";
        if (profit >= 0) {
            std::cout << "  Profit: " << GREEN << "+$" << std::setprecision(4) << profit << RESET << "\n";
        } else {
            std::cout << "  Loss: " << RED << "-$" << std::setprecision(4) << (-profit) << RESET << "\n";
        }
        std::cout << YELLOW << "═══════════════════════════════════════════════════════════════" << RESET << "\n\n";
    }

    void print_status() {
        // Clear line and print status
        std::cout << "\r";

        // BTC Price
        std::cout << DIM << "[" << RESET;
        std::cout << "BTC: " << BOLD << "$" << std::fixed << std::setprecision(2) << btc_price << RESET;

        // Order book
        std::cout << DIM << " | " << RESET;
        std::cout << "YES: " << CYAN << std::setprecision(2) << yes_best_bid << "/" << yes_best_ask << RESET;
        std::cout << " NO: " << MAGENTA << std::setprecision(2) << no_best_bid << "/" << no_best_ask << RESET;

        // Sum of asks (key metric)
        double sum_asks = yes_best_ask + no_best_ask;
        std::cout << DIM << " | " << RESET;
        if (sum_asks < 0.98) {
            std::cout << "Sum: " << GREEN << BOLD << std::setprecision(4) << sum_asks << RESET;
        } else {
            std::cout << "Sum: " << std::setprecision(4) << sum_asks;
        }

        // Balance & PnL
        std::cout << DIM << " | " << RESET;
        std::cout << "Balance: $" << std::setprecision(2) << balance;
        if (total_pnl >= 0) {
            std::cout << " PnL: " << GREEN << "+$" << std::setprecision(2) << total_pnl << RESET;
        } else {
            std::cout << " PnL: " << RED << "-$" << std::setprecision(2) << (-total_pnl) << RESET;
        }

        // Positions
        if (yes_position.quantity > 0) {
            std::cout << DIM << " | " << RESET;
            std::cout << "Pos: " << std::setprecision(1) << yes_position.quantity << " shares";
        }

        std::cout << DIM << "]" << RESET;
        std::cout << std::flush;
    }

    void print_final_report() {
        std::cout << "\n\n";
        std::cout << CYAN << "╔═══════════════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                         TRADING SESSION SUMMARY                                ║\n";
        std::cout << "╚═══════════════════════════════════════════════════════════════════════════════╝" << RESET << "\n\n";

        std::cout << "  Session Statistics:\n";
        std::cout << "  ─────────────────────────────────────────────────────\n";
        std::cout << "  Signals Detected:     " << signals_detected << "\n";
        std::cout << "  Total Trades:         " << total_trades << "\n";
        std::cout << "  Winning Trades:       " << winning_trades << "\n";
        if (total_trades > 0) {
            std::cout << "  Win Rate:             " << std::fixed << std::setprecision(1)
                      << (100.0 * winning_trades / total_trades) << "%\n";
        }
        std::cout << "\n";

        std::cout << "  Financial Summary:\n";
        std::cout << "  ─────────────────────────────────────────────────────\n";
        std::cout << "  Starting Balance:     $50.00\n";
        std::cout << "  Final Balance:        $" << std::setprecision(2) << balance << "\n";
        std::cout << "  Total Fees Paid:      $" << std::setprecision(4) << total_fees << "\n";
        if (total_pnl >= 0) {
            std::cout << "  " << GREEN << BOLD << "NET PROFIT:            +$" << std::setprecision(2) << total_pnl << RESET << "\n";
            std::cout << "  " << GREEN << "Return on Capital:     +" << std::setprecision(1) << (total_pnl / 50.0 * 100) << "%" << RESET << "\n";
        } else {
            std::cout << "  " << RED << BOLD << "NET LOSS:              -$" << std::setprecision(2) << (-total_pnl) << RESET << "\n";
            std::cout << "  " << RED << "Return on Capital:     " << std::setprecision(1) << (total_pnl / 50.0 * 100) << "%" << RESET << "\n";
        }
        std::cout << "\n";
    }

    void run(int duration_seconds = 60) {
        print_banner();

        std::cout << BOLD << "Starting trading simulation... Press Ctrl+C to stop early.\n" << RESET << std::endl;
        std::cout << "Monitoring for arbitrage opportunities...\n\n";

        auto start_time = std::chrono::steady_clock::now();
        int tick = 0;
        int settle_counter = 0;

        while (true) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= duration_seconds) {
                break;
            }

            // Update market state
            update_btc_price();
            update_order_books();

            // Check for opportunities
            double edge, trade_size;
            if (check_underpricing_opportunity(edge, trade_size)) {
                execute_arbitrage_trade(edge, trade_size);
            }

            // Periodically settle positions (simulate market resolution)
            settle_counter++;
            if (settle_counter > 80 && yes_position.quantity > 0) {  // ~8 seconds
                settle_positions();
                settle_counter = 0;
            }

            // Print status every 10 ticks
            if (tick % 10 == 0) {
                print_status();
            }

            tick++;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Final settlement if we have positions
        if (yes_position.quantity > 0) {
            settle_positions();
        }

        print_final_report();
    }
};

int main(int argc, char* argv[]) {
    int duration = 90;  // Default 90 seconds

    if (argc > 1) {
        duration = std::atoi(argv[1]);
        if (duration <= 0) duration = 90;
    }

    std::cout << "\033[2J\033[H";  // Clear screen

    ArbitrageSimulator sim;
    sim.run(duration);

    return 0;
}
