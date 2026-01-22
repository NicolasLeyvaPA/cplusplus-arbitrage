/**
 * HONEST Arbitrage Simulator
 *
 * This simulator is HONEST about what works and what doesn't.
 * It separates TRUE ARBITRAGE from SPECULATION.
 *
 * TRUE ARBITRAGE (zero risk):
 *   - Sum of asks < 99¢ → buy both sides → guaranteed profit
 *   - These opportunities are RARE (maybe 1-2% of the time)
 *   - When they exist, they last milliseconds
 *   - Professional Rust bots with dedicated RPC take them first
 *
 * NOT ARBITRAGE (has risk):
 *   - "Latency arb" - directional bet, can lose
 *   - "Resolution farming" - high probability but NOT certain
 *   - "Stale odds" - prediction, not arbitrage
 *
 * This sim shows the REALITY:
 *   - True arb opportunities are rare
 *   - Most "strategies" are just speculation with fancy names
 *   - Win rate ≠ profitability (can win 99% and still lose money)
 *
 * Compile: g++ -std=c++20 -O3 -o honest_arb honest_arb.cpp -pthread
 */

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <cmath>
#include <random>
#include <chrono>
#include <thread>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

// ANSI codes
namespace ansi {
    const char* CLR = "\033[2J\033[H";
    const char* HIDE = "\033[?25l";
    const char* SHOW = "\033[?25h";
    const char* BOLD = "\033[1m";
    const char* DIM = "\033[2m";
    const char* RST = "\033[0m";
    const char* GRN = "\033[32m";
    const char* RED = "\033[31m";
    const char* YEL = "\033[33m";
    const char* CYN = "\033[36m";
    const char* MAG = "\033[35m";
}

// ============================================================================
// MARKET SIMULATION - Reflects REAL Polymarket behavior
// ============================================================================
struct Market {
    double yes_bid = 0.0;
    double yes_ask = 0.0;
    double no_bid = 0.0;
    double no_ask = 0.0;

    double sum_asks() const { return yes_ask + no_ask; }
    double spread() const { return (yes_ask - yes_bid) + (no_ask - no_bid); }

    // Position
    double yes_qty = 0.0;
    double no_qty = 0.0;
    double total_cost = 0.0;

    bool has_locked_arb() const { return yes_qty > 0 && no_qty > 0; }
    double locked_qty() const { return std::min(yes_qty, no_qty); }

    // Guaranteed profit if we have locked arb (both sides)
    double guaranteed_profit() const {
        if (!has_locked_arb()) return 0.0;
        double qty = locked_qty();
        double payout = qty * 0.99;  // One side pays $1 minus 1% fee
        // We paid total_cost for qty of each side
        double cost_per_pair = total_cost / qty;
        return payout - cost_per_pair * qty;
    }
};

struct Trade {
    std::string time;
    std::string type;
    double price;
    double qty;
    double edge;
    bool is_arb;  // True = guaranteed profit, False = speculation
};

// ============================================================================
// HONEST TRADING ENGINE
// ============================================================================
class HonestEngine {
public:
    HonestEngine(double bal) : balance_(bal), start_balance_(bal) {}

    // State
    double balance() const { return balance_; }
    double start_balance() const { return start_balance_; }
    double realized_pnl() const { return realized_pnl_; }
    double unrealized_pnl() const { return market_.guaranteed_profit(); }
    double true_pnl() const { return balance_ - start_balance_ + unrealized_pnl(); }

    int arb_opportunities_seen() const { return arb_opps_seen_; }
    int arb_trades_taken() const { return arb_trades_; }
    int speculation_trades() const { return spec_trades_; }
    int ticks_waited() const { return ticks_; }

    const Market& market() const { return market_; }
    const std::deque<Trade>& trades() const { return trades_; }
    const std::deque<std::string>& log() const { return log_; }

    void tick() {
        ticks_++;
        update_market();
        check_pure_arbitrage();
        maybe_resolve();
    }

    void force_resolve() {
        if (market_.has_locked_arb()) {
            resolve_arb();
        }
    }

private:
    double balance_;
    double start_balance_;
    double realized_pnl_ = 0.0;

    int arb_opps_seen_ = 0;
    int arb_trades_ = 0;
    int spec_trades_ = 0;
    int ticks_ = 0;
    int ticks_since_entry_ = 0;

    Market market_;
    std::deque<Trade> trades_;
    std::deque<std::string> log_;

    std::mt19937 rng_{std::random_device{}()};

    void add_log(const std::string& msg) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&t));
        log_.push_front(std::string(buf) + " " + msg);
        while (log_.size() > 10) log_.pop_back();
    }

    void update_market() {
        // Simulate a realistic Polymarket order book
        // Key insight: Market makers price efficiently 99%+ of the time

        std::uniform_real_distribution<double> fair_dist(0.30, 0.70);
        std::uniform_real_distribution<double> spread_dist(0.02, 0.04);
        std::uniform_real_distribution<double> arb_chance(0.0, 1.0);

        double fair_yes = fair_dist(rng_);
        double fair_no = 1.0 - fair_yes;
        double spread = spread_dist(rng_);

        market_.yes_bid = fair_yes - spread / 2;
        market_.yes_ask = fair_yes + spread / 2;
        market_.no_bid = fair_no - spread / 2;
        market_.no_ask = fair_no + spread / 2;

        // TRUE ARB OPPORTUNITY: ~2% of the time
        // (In reality it's much rarer and lasts milliseconds)
        if (arb_chance(rng_) < 0.02) {
            // Someone made a mistake or there's temporary inefficiency
            std::uniform_real_distribution<double> discount(0.02, 0.05);
            double d = discount(rng_);

            // Make sum < 0.99
            market_.yes_ask -= d / 2;
            market_.no_ask -= d / 2;

            add_log(std::string(ansi::GRN) + "ARB OPPORTUNITY: Sum = " +
                   std::to_string(market_.sum_asks()).substr(0,5) + ansi::RST);
            arb_opps_seen_++;
        }

        // Clamp to valid range
        market_.yes_bid = std::clamp(market_.yes_bid, 0.01, 0.98);
        market_.yes_ask = std::clamp(market_.yes_ask, 0.02, 0.99);
        market_.no_bid = std::clamp(market_.no_bid, 0.01, 0.98);
        market_.no_ask = std::clamp(market_.no_ask, 0.02, 0.99);
    }

    void check_pure_arbitrage() {
        // ONLY trade if it's TRUE ARBITRAGE
        // Sum of asks < 0.99 means guaranteed profit

        const double THRESHOLD = 0.99;
        const double MIN_EDGE = 0.005;  // 0.5 cent minimum

        double sum = market_.sum_asks();

        if (sum < THRESHOLD - MIN_EDGE && !market_.has_locked_arb()) {
            double edge = THRESHOLD - sum;

            // Position size: max 30% of balance
            double max_spend = balance_ * 0.30;
            double qty = max_spend / sum;
            qty = std::min(qty, 100.0);  // Max 100 contracts

            double yes_cost = market_.yes_ask * qty;
            double no_cost = market_.no_ask * qty;
            double total = yes_cost + no_cost;

            if (total > balance_) return;

            // EXECUTE TRUE ARBITRAGE
            balance_ -= total;
            market_.yes_qty = qty;
            market_.no_qty = qty;
            market_.total_cost = total;
            ticks_since_entry_ = 0;

            Trade t;
            t.time = get_time();
            t.type = "BUY ARB PAIR";
            t.price = sum;
            t.qty = qty;
            t.edge = edge;
            t.is_arb = true;
            trades_.push_front(t);

            arb_trades_++;

            add_log(std::string(ansi::GRN) + ansi::BOLD +
                   "EXECUTED: Bought pair @ " + std::to_string(sum).substr(0,5) +
                   " Edge: " + std::to_string(edge * 100).substr(0,4) + "%" + ansi::RST);

            while (trades_.size() > 10) trades_.pop_back();
        }
    }

    void maybe_resolve() {
        if (!market_.has_locked_arb()) return;

        ticks_since_entry_++;

        // Resolve after ~50 ticks (5 seconds) to simulate market settlement
        if (ticks_since_entry_ >= 50) {
            resolve_arb();
        }
    }

    void resolve_arb() {
        double qty = market_.locked_qty();
        double payout = qty * 0.99;  // One side wins, pays $1 - 1% fee
        double profit = payout - market_.total_cost;

        balance_ += payout;
        realized_pnl_ += profit;

        Trade t;
        t.time = get_time();
        t.type = "SETTLE ARB";
        t.price = 0.99;
        t.qty = qty;
        t.edge = profit;
        t.is_arb = true;
        trades_.push_front(t);

        add_log(std::string(profit >= 0 ? ansi::GRN : ansi::RED) +
               "SETTLED: Profit = $" + std::to_string(profit).substr(0,5) + ansi::RST);

        // Clear position
        market_.yes_qty = market_.no_qty = market_.total_cost = 0;

        while (trades_.size() > 10) trades_.pop_back();
    }

    std::string get_time() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&t));
        return std::string(buf);
    }
};

// ============================================================================
// DASHBOARD
// ============================================================================
class Dashboard {
public:
    void render(const HonestEngine& engine, int elapsed_sec, int total_sec) {
        std::ostringstream out;
        out << ansi::CLR;

        // Header
        out << ansi::BOLD << ansi::CYN;
        out << "╔══════════════════════════════════════════════════════════════════════╗\n";
        out << "║           HONEST ARBITRAGE SIMULATOR - No BS Edition                 ║\n";
        out << "╚══════════════════════════════════════════════════════════════════════╝\n";
        out << ansi::RST << "\n";

        // Progress
        double pct = 100.0 * elapsed_sec / total_sec;
        out << "  Time: " << elapsed_sec << "s / " << total_sec << "s ("
            << std::fixed << std::setprecision(0) << pct << "%)";
        out << "   Ticks: " << engine.ticks_waited() << "\n\n";

        // Market State
        const auto& mkt = engine.market();
        out << "┌─ MARKET STATE ──────────────────────────────────────────────────────┐\n";
        out << "│  YES: " << std::fixed << std::setprecision(4)
            << mkt.yes_bid << " / " << mkt.yes_ask;
        out << "    NO: " << mkt.no_bid << " / " << mkt.no_ask;
        out << "    SUM: ";

        double sum = mkt.sum_asks();
        if (sum < 0.99) {
            out << ansi::GRN << ansi::BOLD << sum << " ← ARB!" << ansi::RST;
        } else {
            out << ansi::DIM << sum << " (no arb)" << ansi::RST;
        }
        out << "   │\n";
        out << "└─────────────────────────────────────────────────────────────────────┘\n\n";

        // Position
        out << "┌─ POSITION ──────────────────────────────────────────────────────────┐\n";
        if (mkt.has_locked_arb()) {
            out << "│  " << ansi::GRN << "LOCKED ARB PAIR" << ansi::RST;
            out << "   Qty: " << std::setprecision(1) << mkt.locked_qty();
            out << "   Cost: $" << std::setprecision(2) << mkt.total_cost;
            out << "   Guaranteed: " << ansi::GRN << "+$"
                << std::setprecision(2) << mkt.guaranteed_profit() << ansi::RST;
            out << "    │\n";
        } else {
            out << "│  " << ansi::DIM << "(no position - waiting for true arb opportunity)"
                << ansi::RST << "                │\n";
        }
        out << "└─────────────────────────────────────────────────────────────────────┘\n\n";

        // P&L (The important part)
        out << "┌─ P&L ───────────────────────────────────────────────────────────────┐\n";
        out << "│  Balance: $" << std::setprecision(2) << engine.balance();
        out << "   Realized: ";
        double real = engine.realized_pnl();
        if (real >= 0) out << ansi::GRN << "+$" << real;
        else out << ansi::RED << "-$" << std::abs(real);
        out << ansi::RST;
        out << "   Unrealized: ";
        double unreal = engine.unrealized_pnl();
        if (unreal >= 0) out << ansi::GRN << "+$" << unreal;
        else out << ansi::RED << "-$" << std::abs(unreal);
        out << ansi::RST << "           │\n";

        out << "│  " << ansi::BOLD << "TRUE P&L: ";
        double pnl = engine.true_pnl();
        if (pnl >= 0) out << ansi::GRN << "+$" << pnl;
        else out << ansi::RED << "-$" << std::abs(pnl);
        out << ansi::RST;
        double ret = (pnl / engine.start_balance()) * 100;
        out << " (" << (ret >= 0 ? "+" : "") << std::setprecision(1) << ret << "%)";
        out << ansi::RST << "                                              │\n";
        out << "└─────────────────────────────────────────────────────────────────────┘\n\n";

        // Statistics
        out << "┌─ STATISTICS ────────────────────────────────────────────────────────┐\n";
        out << "│  Arb Opportunities Seen: " << engine.arb_opportunities_seen();
        out << "   Arb Trades Taken: " << engine.arb_trades_taken();
        out << "   Win Rate: 100%";
        out << "         │\n";
        out << "└─────────────────────────────────────────────────────────────────────┘\n\n";

        // Activity Log
        out << "┌─ ACTIVITY LOG ──────────────────────────────────────────────────────┐\n";
        const auto& log = engine.log();
        for (int i = 0; i < 6; i++) {
            out << "│  ";
            if (i < static_cast<int>(log.size())) {
                out << log[i];
            }
            out << "\033[K│\n";  // Clear to end of line
        }
        out << "└─────────────────────────────────────────────────────────────────────┘\n\n";

        // Educational note
        out << ansi::DIM;
        out << "  REALITY CHECK:\n";
        out << "  • TRUE arb (sum < 99¢) happens ~2% of ticks in this sim\n";
        out << "  • In real Polymarket, it's MUCH rarer and lasts milliseconds\n";
        out << "  • Professional Rust bots with dedicated RPC take these first\n";
        out << "  • \"Latency arb\" and \"stale odds\" are NOT true arbitrage - they're speculation\n";
        out << ansi::RST;

        std::cout << out.str() << std::flush;
    }

    void render_final(const HonestEngine& engine) {
        std::cout << ansi::CLR;
        std::cout << ansi::BOLD << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                    HONEST RESULTS - NO BS                            ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════════╝\n";
        std::cout << ansi::RST << "\n";

        std::cout << "  Starting Balance:  $" << std::fixed << std::setprecision(2)
                  << engine.start_balance() << "\n";
        std::cout << "  Final Balance:     $" << engine.balance() << "\n";
        std::cout << "  Realized P&L:      $" << engine.realized_pnl() << "\n\n";

        double pnl = engine.true_pnl();
        std::cout << ansi::BOLD << "  ════════════════════════════════════════════\n";
        std::cout << "  TRUE NET PROFIT:   ";
        if (pnl >= 0) std::cout << ansi::GRN << "+$" << pnl;
        else std::cout << ansi::RED << "-$" << std::abs(pnl);
        std::cout << ansi::RST << ansi::BOLD << "\n";

        double ret = (pnl / engine.start_balance()) * 100;
        std::cout << "  RETURN:            ";
        if (ret >= 0) std::cout << ansi::GRN << "+" << std::setprecision(1) << ret << "%";
        else std::cout << ansi::RED << ret << "%";
        std::cout << ansi::RST << ansi::BOLD << "\n";
        std::cout << "  ════════════════════════════════════════════" << ansi::RST << "\n\n";

        std::cout << "  Arb Opportunities Seen:  " << engine.arb_opportunities_seen() << "\n";
        std::cout << "  Arb Trades Executed:     " << engine.arb_trades_taken() << "\n";
        std::cout << "  Win Rate:                100% (true arb = guaranteed)\n\n";

        std::cout << ansi::YEL << "  KEY INSIGHTS:\n";
        std::cout << "  • Only traded TRUE arbitrage (sum < 99¢)\n";
        std::cout << "  • Avoided speculation disguised as \"strategies\"\n";
        std::cout << "  • In real markets, these opportunities are rarer and faster\n";
        std::cout << "  • Professional bots with better infra capture most of them\n";
        std::cout << ansi::RST << "\n";
    }
};

// ============================================================================
// INPUT
// ============================================================================
class Input {
public:
    Input() {
        tcgetattr(STDIN_FILENO, &old_);
        termios t = old_;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    ~Input() { tcsetattr(STDIN_FILENO, TCSANOW, &old_); }
    char get() {
        char c = 0;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        timeval tv = {0, 0};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
            read(STDIN_FILENO, &c, 1);
        }
        return c;
    }
private:
    termios old_;
};

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char* argv[]) {
    int duration = argc > 1 ? std::stoi(argv[1]) : 60;  // Default 60 seconds

    HonestEngine engine(50.0);
    Dashboard dash;
    Input input;

    std::cout << ansi::HIDE;

    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();

        if (elapsed >= duration) break;

        char key = input.get();
        if (key == 'q' || key == 'Q') break;

        engine.tick();
        dash.render(engine, elapsed, duration);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Force resolve any open positions
    engine.force_resolve();

    std::cout << ansi::SHOW;
    dash.render_final(engine);

    return 0;
}
