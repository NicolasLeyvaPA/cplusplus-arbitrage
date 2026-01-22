/**
 * DailyArb Dashboard Trader v2.0
 *
 * STATE-BASED UI - No infinite scroll!
 * - Fixed-position dashboard that updates in place
 * - Clear PnL visualization: Balance, True PnL, Unrealized, Exposure
 * - Trade log shows last 8 trades (fixed height)
 * - Operator controls: [P]ause, [R]esume, [Q]uit
 *
 * AGGRESSIVE 5-MINUTE STRATEGY
 * - Lower thresholds for more frequent signals
 * - Multiple strategy modes for testing
 *
 * Compile: g++ -std=c++20 -O3 -o dashboard_trader dashboard_trader.cpp -pthread
 * Run: ./dashboard_trader [duration_seconds]
 */

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <array>
#include <cmath>
#include <ctime>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

// ============================================================================
// ANSI ESCAPE CODES FOR DASHBOARD
// ============================================================================
namespace ansi {
    const char* CLEAR_SCREEN = "\033[2J";
    const char* HOME = "\033[H";
    const char* HIDE_CURSOR = "\033[?25l";
    const char* SHOW_CURSOR = "\033[?25h";
    const char* BOLD = "\033[1m";
    const char* RESET = "\033[0m";
    const char* GREEN = "\033[32m";
    const char* RED = "\033[31m";
    const char* YELLOW = "\033[33m";
    const char* CYAN = "\033[36m";
    const char* MAGENTA = "\033[35m";
    const char* DIM = "\033[2m";
    const char* BG_BLUE = "\033[44m";
    const char* WHITE = "\033[37m";

    std::string move_to(int row, int col) {
        return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
    }

    std::string clear_line() {
        return "\033[2K";
    }
}

// ============================================================================
// CONFIGURATION
// ============================================================================
struct Config {
    // Strategy parameters - AGGRESSIVE for 5-minute windows
    double btc_move_threshold_bps = 5.0;     // Was 15, now 5 for more signals
    double staleness_threshold_ms = 200.0;   // Was 500, now 200
    double min_edge_bps = 3.0;               // Was 10, now 3
    double take_profit_bps = 8.0;            // Was 20, now 8
    double stop_loss_bps = 5.0;              // Was 15, now 5
    double max_hold_ms = 10000.0;            // Was 30s, now 10s

    // Position sizing
    double starting_balance = 50.0;
    double position_size_pct = 10.0;         // 10% of balance per trade
    double max_positions = 5;

    // Fees
    double fee_rate = 0.02;                  // 2% Polymarket fee

    // UI refresh rate
    int refresh_ms = 200;                    // 5 FPS dashboard refresh
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================
struct Trade {
    uint64_t timestamp_ms;
    std::string action;      // "BUY YES", "BUY NO", "SELL", "SETTLE"
    double price;
    double quantity;
    double pnl;              // This trade's realized P&L
    double balance_after;    // Balance after this trade
    std::string reason;      // Why trade was made
};

struct Position {
    std::string side;        // "YES" or "NO"
    double entry_price;
    double quantity;
    double current_price;
    uint64_t entry_time;

    double unrealized_pnl() const {
        return (current_price - entry_price) * quantity;
    }

    double notional() const {
        return entry_price * quantity;
    }
};

struct MarketState {
    double btc_price = 104500.0;
    double btc_price_5s_ago = 104500.0;
    double btc_move_bps = 0.0;

    double yes_bid = 0.48;
    double yes_ask = 0.52;
    double no_bid = 0.48;
    double no_ask = 0.52;

    uint64_t last_book_update = 0;
    uint64_t staleness_ms = 0;

    bool is_stale() const { return staleness_ms > 200; }
};

enum class SystemState {
    RUNNING,
    PAUSED,
    STOPPED
};

// ============================================================================
// PRICE HISTORY TRACKER
// ============================================================================
class PriceHistory {
public:
    void add(double price, uint64_t time_ms) {
        history_.push_back({price, time_ms});
        // Keep last 100 entries
        while (history_.size() > 100) {
            history_.pop_front();
        }
    }

    double price_at(uint64_t lookback_ms, uint64_t current_time) const {
        uint64_t target_time = current_time - lookback_ms;
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            if (it->time_ms <= target_time) {
                return it->price;
            }
        }
        return history_.empty() ? 0.0 : history_.front().price;
    }

private:
    struct Entry { double price; uint64_t time_ms; };
    std::deque<Entry> history_;
};

// ============================================================================
// TRADING ENGINE
// ============================================================================
class TradingEngine {
public:
    explicit TradingEngine(const Config& cfg)
        : config_(cfg), balance_(cfg.starting_balance), state_(SystemState::RUNNING) {}

    // Accessors
    double balance() const { return balance_; }
    double starting_balance() const { return config_.starting_balance; }
    double true_pnl() const { return balance_ - config_.starting_balance + realized_pnl_; }
    double realized_pnl() const { return realized_pnl_; }
    double unrealized_pnl() const {
        double pnl = 0.0;
        for (const auto& pos : positions_) {
            pnl += pos.unrealized_pnl();
        }
        return pnl;
    }
    double total_exposure() const {
        double exp = 0.0;
        for (const auto& pos : positions_) {
            exp += pos.notional();
        }
        return exp;
    }
    int trade_count() const { return trade_count_; }
    int signal_count() const { return signal_count_; }
    int win_count() const { return win_count_; }
    int loss_count() const { return loss_count_; }
    double win_rate() const {
        int total = win_count_ + loss_count_;
        return total > 0 ? (double)win_count_ / total * 100.0 : 0.0;
    }
    const std::deque<Trade>& recent_trades() const { return trades_; }
    const std::vector<Position>& positions() const { return positions_; }
    const MarketState& market() const { return market_; }
    SystemState state() const { return state_; }

    void pause() { state_ = SystemState::PAUSED; }
    void resume() { state_ = SystemState::RUNNING; }
    void stop() { state_ = SystemState::STOPPED; }

    void tick(uint64_t now_ms) {
        if (state_ != SystemState::RUNNING) return;

        // Update market simulation
        update_market(now_ms);

        // Update position prices
        update_positions(now_ms);

        // Check for exits on existing positions
        check_exits(now_ms);

        // Check for new signals
        check_signals(now_ms);
    }

    void settle_all(uint64_t now_ms) {
        // Force close all positions at current market
        for (auto& pos : positions_) {
            double exit_price = (pos.side == "YES") ? market_.yes_bid : market_.no_bid;
            double gross_pnl = (exit_price - pos.entry_price) * pos.quantity;
            double fee = exit_price * pos.quantity * config_.fee_rate;
            double net_pnl = gross_pnl - fee;

            balance_ += exit_price * pos.quantity - fee;
            realized_pnl_ += net_pnl;

            if (net_pnl > 0) win_count_++; else loss_count_++;

            record_trade(now_ms, "SETTLE", exit_price, pos.quantity, net_pnl, "Session end");
        }
        positions_.clear();
    }

private:
    Config config_;
    double balance_;
    double realized_pnl_ = 0.0;
    int trade_count_ = 0;
    int signal_count_ = 0;
    int win_count_ = 0;
    int loss_count_ = 0;

    std::vector<Position> positions_;
    std::deque<Trade> trades_;
    MarketState market_;
    PriceHistory btc_history_;
    SystemState state_;

    std::mt19937 rng_{std::random_device{}()};

    void update_market(uint64_t now_ms) {
        // Simulate BTC price movement (random walk with momentum)
        std::normal_distribution<double> btc_move(-0.0001, 0.0003);  // More volatile
        double move = btc_move(rng_);
        market_.btc_price *= (1.0 + move);

        btc_history_.add(market_.btc_price, now_ms);
        market_.btc_price_5s_ago = btc_history_.price_at(5000, now_ms);

        if (market_.btc_price_5s_ago > 0) {
            market_.btc_move_bps = ((market_.btc_price / market_.btc_price_5s_ago) - 1.0) * 10000.0;
        }

        // Simulate order book updates
        // Book updates ~80% of the time, sometimes stale
        std::uniform_real_distribution<double> book_update_chance(0.0, 1.0);
        if (book_update_chance(rng_) < 0.80) {
            // Update book based on BTC move
            double base = 0.50;
            double adjustment = market_.btc_move_bps * 0.001;  // 1bp BTC = 0.001 odds change

            market_.yes_bid = std::clamp(base + adjustment - 0.02, 0.01, 0.98);
            market_.yes_ask = std::clamp(base + adjustment + 0.02, 0.02, 0.99);
            market_.no_bid = std::clamp(base - adjustment - 0.02, 0.01, 0.98);
            market_.no_ask = std::clamp(base - adjustment + 0.02, 0.02, 0.99);

            market_.last_book_update = now_ms;
            market_.staleness_ms = 0;
        } else {
            market_.staleness_ms = now_ms - market_.last_book_update;
        }
    }

    void update_positions(uint64_t now_ms) {
        for (auto& pos : positions_) {
            if (pos.side == "YES") {
                pos.current_price = market_.yes_bid;  // Mark at bid (exit price)
            } else {
                pos.current_price = market_.no_bid;
            }
        }
    }

    void check_exits(uint64_t now_ms) {
        std::vector<Position> remaining;

        for (auto& pos : positions_) {
            double pnl_bps = ((pos.current_price / pos.entry_price) - 1.0) * 10000.0;
            uint64_t hold_time = now_ms - pos.entry_time;

            bool should_exit = false;
            std::string exit_reason;

            if (pnl_bps >= config_.take_profit_bps) {
                should_exit = true;
                exit_reason = "Take profit";
            } else if (pnl_bps <= -config_.stop_loss_bps) {
                should_exit = true;
                exit_reason = "Stop loss";
            } else if (hold_time >= config_.max_hold_ms) {
                should_exit = true;
                exit_reason = "Timeout";
            }

            if (should_exit) {
                double exit_price = pos.current_price;
                double gross_pnl = (exit_price - pos.entry_price) * pos.quantity;
                double fee = exit_price * pos.quantity * config_.fee_rate;
                double net_pnl = gross_pnl - fee;

                balance_ += exit_price * pos.quantity - fee;
                realized_pnl_ += net_pnl;

                if (net_pnl > 0) win_count_++; else loss_count_++;

                record_trade(now_ms, "SELL " + pos.side, exit_price, pos.quantity, net_pnl, exit_reason);
            } else {
                remaining.push_back(pos);
            }
        }

        positions_ = remaining;
    }

    void check_signals(uint64_t now_ms) {
        // Don't open more positions if at limit
        if (positions_.size() >= static_cast<size_t>(config_.max_positions)) return;

        // S1: Stale Odds Strategy
        // If BTC moved but book is stale, the market is mispriced
        bool btc_moved = std::abs(market_.btc_move_bps) >= config_.btc_move_threshold_bps;
        bool book_stale = market_.staleness_ms >= config_.staleness_threshold_ms;

        if (btc_moved && book_stale) {
            signal_count_++;

            // Determine direction
            std::string side;
            double entry_price;

            if (market_.btc_move_bps > 0) {
                // BTC up -> YES should be higher -> buy YES (it's stale/cheap)
                side = "YES";
                entry_price = market_.yes_ask;
            } else {
                // BTC down -> NO should be higher -> buy NO
                side = "NO";
                entry_price = market_.no_ask;
            }

            // Position sizing
            double position_value = balance_ * (config_.position_size_pct / 100.0);
            double quantity = position_value / entry_price;
            double cost = entry_price * quantity;

            if (cost > balance_) return;  // Not enough balance

            // Execute entry
            balance_ -= cost;

            Position pos;
            pos.side = side;
            pos.entry_price = entry_price;
            pos.quantity = quantity;
            pos.current_price = entry_price;
            pos.entry_time = now_ms;
            positions_.push_back(pos);

            std::string reason = "Stale " + std::to_string(static_cast<int>(market_.staleness_ms)) +
                                "ms, BTC " + (market_.btc_move_bps > 0 ? "+" : "") +
                                std::to_string(static_cast<int>(market_.btc_move_bps)) + "bps";
            record_trade(now_ms, "BUY " + side, entry_price, quantity, 0.0, reason);
        }
    }

    void record_trade(uint64_t ts, const std::string& action, double price,
                      double qty, double pnl, const std::string& reason) {
        Trade t;
        t.timestamp_ms = ts;
        t.action = action;
        t.price = price;
        t.quantity = qty;
        t.pnl = pnl;
        t.balance_after = balance_;
        t.reason = reason;

        trades_.push_front(t);
        trade_count_++;

        // Keep last 20 trades
        while (trades_.size() > 20) {
            trades_.pop_back();
        }
    }
};

// ============================================================================
// DASHBOARD RENDERER
// ============================================================================
class Dashboard {
public:
    void render(const TradingEngine& engine, uint64_t elapsed_ms, uint64_t total_ms) {
        std::ostringstream out;

        // Move to home and clear
        out << ansi::HOME;

        // Header
        out << ansi::BOLD << ansi::BG_BLUE << ansi::WHITE;
        out << "╔══════════════════════════════════════════════════════════════════════════╗\n";
        out << "║              DAILYARB DASHBOARD v2.0 - STATE-BASED VIEW                  ║\n";
        out << "╚══════════════════════════════════════════════════════════════════════════╝";
        out << ansi::RESET << "\n\n";

        // State indicator
        out << "  Status: ";
        switch (engine.state()) {
            case SystemState::RUNNING:
                out << ansi::GREEN << ansi::BOLD << "● RUNNING" << ansi::RESET;
                break;
            case SystemState::PAUSED:
                out << ansi::YELLOW << ansi::BOLD << "◐ PAUSED" << ansi::RESET;
                break;
            case SystemState::STOPPED:
                out << ansi::RED << ansi::BOLD << "■ STOPPED" << ansi::RESET;
                break;
        }

        // Progress bar
        double progress = (double)elapsed_ms / total_ms;
        int bar_width = 30;
        int filled = static_cast<int>(progress * bar_width);
        out << "    Time: [";
        for (int i = 0; i < bar_width; i++) {
            out << (i < filled ? "█" : "░");
        }
        out << "] " << std::fixed << std::setprecision(0) << (progress * 100) << "%";
        out << "  (" << (elapsed_ms / 1000) << "s / " << (total_ms / 1000) << "s)\n\n";

        // Controls hint
        out << ansi::DIM << "  Controls: [P]ause  [R]esume  [Q]uit" << ansi::RESET << "\n\n";

        // Market State Box
        const auto& mkt = engine.market();
        out << "┌─────────────────────────── MARKET STATE ───────────────────────────────┐\n";
        out << "│  BTC: " << ansi::CYAN << ansi::BOLD << "$" << std::fixed << std::setprecision(2)
            << mkt.btc_price << ansi::RESET;
        out << "    5s Move: ";
        if (mkt.btc_move_bps > 0) out << ansi::GREEN << "+";
        else if (mkt.btc_move_bps < 0) out << ansi::RED;
        else out << ansi::DIM;
        out << std::fixed << std::setprecision(1) << mkt.btc_move_bps << " bps" << ansi::RESET;
        out << "    Book Stale: ";
        if (mkt.staleness_ms > 200) out << ansi::YELLOW << mkt.staleness_ms << "ms" << ansi::RESET;
        else out << ansi::DIM << mkt.staleness_ms << "ms" << ansi::RESET;
        out << "       │\n";

        out << "│  YES: " << std::setprecision(4) << mkt.yes_bid << "/" << mkt.yes_ask;
        out << "    NO: " << mkt.no_bid << "/" << mkt.no_ask;
        out << "    Sum: " << std::setprecision(4) << (mkt.yes_ask + mkt.no_ask);
        out << "                            │\n";
        out << "└────────────────────────────────────────────────────────────────────────┘\n\n";

        // P&L Box - THE KEY STATE VIEW
        out << "┌─────────────────────────── P&L STATE ──────────────────────────────────┐\n";

        // Balance
        out << "│  " << ansi::BOLD << "Balance:" << ansi::RESET << "      $"
            << std::fixed << std::setprecision(2) << engine.balance();
        out << "   (started: $" << engine.starting_balance() << ")";
        out << "                              │\n";

        // True P&L (most important number)
        double true_pnl = engine.balance() + engine.unrealized_pnl() - engine.starting_balance();
        out << "│  " << ansi::BOLD << "TRUE P&L:" << ansi::RESET << "     ";
        if (true_pnl >= 0) out << ansi::GREEN << "+$" << true_pnl << ansi::RESET;
        else out << ansi::RED << "-$" << std::abs(true_pnl) << ansi::RESET;
        out << "  (";
        double pct = (true_pnl / engine.starting_balance()) * 100.0;
        if (pct >= 0) out << ansi::GREEN << "+" << std::setprecision(1) << pct << "%" << ansi::RESET;
        else out << ansi::RED << std::setprecision(1) << pct << "%" << ansi::RESET;
        out << ")                                       │\n";

        // Realized vs Unrealized
        out << "│  " << ansi::DIM << "Realized:" << ansi::RESET << "     ";
        if (engine.realized_pnl() >= 0) out << ansi::GREEN << "+$" << std::setprecision(2) << engine.realized_pnl();
        else out << ansi::RED << "-$" << std::setprecision(2) << std::abs(engine.realized_pnl());
        out << ansi::RESET << "    ";

        out << ansi::DIM << "Unrealized:" << ansi::RESET << " ";
        if (engine.unrealized_pnl() >= 0) out << ansi::GREEN << "+$" << std::setprecision(2) << engine.unrealized_pnl();
        else out << ansi::RED << "-$" << std::setprecision(2) << std::abs(engine.unrealized_pnl());
        out << ansi::RESET << "                         │\n";

        // Exposure
        out << "│  " << ansi::DIM << "Exposure:" << ansi::RESET << "     $" << std::setprecision(2) << engine.total_exposure();
        out << " (" << engine.positions().size() << " positions)";
        out << "                                      │\n";

        out << "└────────────────────────────────────────────────────────────────────────┘\n\n";

        // Statistics Box
        out << "┌─────────────────────────── STATISTICS ─────────────────────────────────┐\n";
        out << "│  Signals: " << engine.signal_count();
        out << "    Trades: " << engine.trade_count();
        out << "    Win/Loss: " << engine.win_count() << "/" << engine.loss_count();
        out << "    Win Rate: " << std::setprecision(1) << engine.win_rate() << "%";
        out << "            │\n";
        out << "└────────────────────────────────────────────────────────────────────────┘\n\n";

        // Trade Log (fixed 8 rows)
        out << "┌─────────────────────────── RECENT TRADES ──────────────────────────────┐\n";

        const auto& trades = engine.recent_trades();
        for (int i = 0; i < 8; i++) {
            out << "│ ";
            if (i < static_cast<int>(trades.size())) {
                const auto& t = trades[i];

                // Time
                time_t sec = t.timestamp_ms / 1000;
                struct tm* tm_info = localtime(&sec);
                char time_buf[16];
                strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);
                out << time_buf << "  ";

                // Action with color
                if (t.action.find("BUY") != std::string::npos) {
                    out << ansi::CYAN << std::left << std::setw(10) << t.action << ansi::RESET;
                } else if (t.action.find("SELL") != std::string::npos) {
                    out << ansi::MAGENTA << std::left << std::setw(10) << t.action << ansi::RESET;
                } else {
                    out << ansi::YELLOW << std::left << std::setw(10) << t.action << ansi::RESET;
                }

                // Price and quantity
                out << " @" << std::fixed << std::setprecision(4) << t.price;
                out << " x" << std::setprecision(1) << t.quantity;

                // P&L
                out << "  P&L: ";
                if (t.pnl > 0) out << ansi::GREEN << "+$" << std::setprecision(2) << t.pnl;
                else if (t.pnl < 0) out << ansi::RED << "-$" << std::setprecision(2) << std::abs(t.pnl);
                else out << ansi::DIM << "$0.00";
                out << ansi::RESET;

                // Reason (truncated)
                std::string reason = t.reason.substr(0, 20);
                out << "  " << ansi::DIM << reason << ansi::RESET;
            }
            // Pad to fixed width
            out << ansi::clear_line();
            out << "│\n";
        }
        out << "└────────────────────────────────────────────────────────────────────────┘\n";

        // Open Positions Box
        out << "\n┌─────────────────────────── OPEN POSITIONS ─────────────────────────────┐\n";
        const auto& positions = engine.positions();
        if (positions.empty()) {
            out << "│  " << ansi::DIM << "(no open positions)" << ansi::RESET;
            out << "                                                   │\n";
        } else {
            for (const auto& pos : positions) {
                out << "│  " << pos.side << ": ";
                out << std::fixed << std::setprecision(1) << pos.quantity << " @ $" << std::setprecision(4) << pos.entry_price;
                out << "  Current: $" << pos.current_price;
                out << "  Unrealized: ";
                double pnl = pos.unrealized_pnl();
                if (pnl >= 0) out << ansi::GREEN << "+$" << std::setprecision(2) << pnl;
                else out << ansi::RED << "-$" << std::setprecision(2) << std::abs(pnl);
                out << ansi::RESET;
                out << "           │\n";
            }
        }
        out << "└────────────────────────────────────────────────────────────────────────┘\n";

        std::cout << out.str() << std::flush;
    }

    void render_final(const TradingEngine& engine) {
        std::cout << ansi::CLEAR_SCREEN << ansi::HOME;
        std::cout << ansi::BOLD << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                    DAILYARB SESSION COMPLETE                             ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════════════╝\n";
        std::cout << ansi::RESET << "\n";

        std::cout << "  Starting Balance:  $" << std::fixed << std::setprecision(2) << engine.starting_balance() << "\n";
        std::cout << "  Final Balance:     $" << engine.balance() << "\n";
        std::cout << "  Unrealized P&L:    $" << engine.unrealized_pnl() << "\n";
        std::cout << "\n";

        double true_pnl = engine.balance() + engine.unrealized_pnl() - engine.starting_balance();
        std::cout << ansi::BOLD << "  ════════════════════════════════════════\n";
        std::cout << "  TRUE NET PROFIT:   ";
        if (true_pnl >= 0) {
            std::cout << ansi::GREEN << "+$" << true_pnl;
        } else {
            std::cout << ansi::RED << "-$" << std::abs(true_pnl);
        }
        std::cout << ansi::RESET << ansi::BOLD << "\n";
        std::cout << "  RETURN:            ";
        double ret = (true_pnl / engine.starting_balance()) * 100.0;
        if (ret >= 0) {
            std::cout << ansi::GREEN << "+" << std::setprecision(1) << ret << "%";
        } else {
            std::cout << ansi::RED << std::setprecision(1) << ret << "%";
        }
        std::cout << ansi::RESET << ansi::BOLD << "\n";
        std::cout << "  ════════════════════════════════════════" << ansi::RESET << "\n\n";

        std::cout << "  Signals Generated: " << engine.signal_count() << "\n";
        std::cout << "  Total Trades:      " << engine.trade_count() << "\n";
        std::cout << "  Win/Loss:          " << engine.win_count() << "/" << engine.loss_count() << "\n";
        std::cout << "  Win Rate:          " << std::setprecision(1) << engine.win_rate() << "%\n\n";
    }
};

// ============================================================================
// NON-BLOCKING INPUT
// ============================================================================
class InputHandler {
public:
    InputHandler() {
        // Save terminal settings
        tcgetattr(STDIN_FILENO, &old_term_);
        termios new_term = old_term_;
        new_term.c_lflag &= ~(ICANON | ECHO);
        new_term.c_cc[VMIN] = 0;
        new_term.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
    }

    ~InputHandler() {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term_);
    }

    char get_key() {
        char c = 0;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);

        struct timeval tv = {0, 0};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
            if (read(STDIN_FILENO, &c, 1) == 1) {
                return c;
            }
        }
        return 0;
    }

private:
    termios old_term_;
};

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char* argv[]) {
    // Duration in seconds (default 5 minutes = 300 seconds)
    int duration_sec = 300;
    if (argc > 1) {
        duration_sec = std::stoi(argv[1]);
    }

    uint64_t duration_ms = static_cast<uint64_t>(duration_sec) * 1000;

    Config config;
    TradingEngine engine(config);
    Dashboard dashboard;
    InputHandler input;

    // Get start time
    auto start = std::chrono::steady_clock::now();
    auto get_elapsed = [&start]() -> uint64_t {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    };

    // Setup terminal
    std::cout << ansi::HIDE_CURSOR << ansi::CLEAR_SCREEN;

    // Main loop
    while (true) {
        uint64_t elapsed = get_elapsed();

        // Check for timeout
        if (elapsed >= duration_ms && engine.state() != SystemState::PAUSED) {
            break;
        }

        // Handle input
        char key = input.get_key();
        if (key == 'q' || key == 'Q') {
            break;
        } else if (key == 'p' || key == 'P') {
            engine.pause();
        } else if (key == 'r' || key == 'R') {
            engine.resume();
        }

        // Tick the engine
        uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        engine.tick(now_ms);

        // Render dashboard
        dashboard.render(engine, elapsed, duration_ms);

        // Sleep for refresh interval
        std::this_thread::sleep_for(std::chrono::milliseconds(config.refresh_ms));
    }

    // Settle remaining positions
    uint64_t final_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    engine.settle_all(final_time);

    // Show cursor and render final results
    std::cout << ansi::SHOW_CURSOR;
    dashboard.render_final(engine);

    return 0;
}
