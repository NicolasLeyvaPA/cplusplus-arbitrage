#include "ui/terminal_ui.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstdio>

namespace arb {

TerminalUI::TerminalUI() {}

void TerminalUI::clear_screen() {
    std::cout << "\033[2J\033[H";
}

void TerminalUI::print_line(char c, int width) {
    for (int i = 0; i < width; i++) std::cout << c;
    std::cout << "\n";
}

void TerminalUI::print_header(TradingMode mode, bool kill_switch_active) {
    print_line('=', 80);
    std::cout << "  DELTA-NEUTRAL FUNDING ARBITRAGE BOT";
    std::cout << "  |  Mode: " << arb::to_string(mode);
    if (kill_switch_active) {
        std::cout << "  |  *** KILL SWITCH ACTIVE ***";
    }
    std::cout << "\n";
    print_line('=', 80);
}

void TerminalUI::print_funding_table(const std::map<std::string, FundingInfo>& funding) {
    std::cout << "\n  FUNDING RATES\n";
    print_line('-', 80);
    std::cout << std::left
              << std::setw(12) << "  Symbol"
              << std::right
              << std::setw(12) << "Rate"
              << std::setw(12) << "APY"
              << std::setw(14) << "Mark Price"
              << std::setw(14) << "Index Price"
              << std::setw(16) << "Next Funding"
              << "\n";
    print_line('-', 80);

    for (const auto& [symbol, info] : funding) {
        std::cout << std::left << "  " << std::setw(10) << symbol
                  << std::right << std::fixed
                  << std::setw(11) << std::setprecision(4) << (info.current_rate * 100) << "%"
                  << std::setw(10) << std::setprecision(1) << info.annualized_rate() << "%"
                  << std::setw(14) << std::setprecision(2) << info.mark_price
                  << std::setw(14) << std::setprecision(2) << info.index_price;

        // Time until next funding
        if (info.next_funding_time_ms > 0) {
            int64_t ms_left = static_cast<int64_t>(info.next_funding_time_ms) - now_ms();
            if (ms_left > 0) {
                int hours = static_cast<int>(ms_left / 3600000);
                int mins = static_cast<int>((ms_left % 3600000) / 60000);
                std::cout << std::setw(12) << "" << hours << "h" << mins << "m";
            }
        }
        std::cout << "\n";
    }
}

void TerminalUI::print_basis_table(const std::map<std::string, BasisInfo>& basis) {
    std::cout << "\n  SPOT-FUTURES BASIS\n";
    print_line('-', 80);
    std::cout << std::left
              << std::setw(12) << "  Symbol"
              << std::right
              << std::setw(14) << "Spot"
              << std::setw(14) << "Futures"
              << std::setw(12) << "Basis bps"
              << std::setw(14) << "Ann. Basis"
              << "\n";
    print_line('-', 80);

    for (const auto& [symbol, info] : basis) {
        std::cout << std::left << "  " << std::setw(10) << symbol
                  << std::right << std::fixed
                  << std::setw(14) << std::setprecision(2) << info.spot_mid
                  << std::setw(14) << std::setprecision(2) << info.futures_mid
                  << std::setw(12) << std::setprecision(2) << info.basis_bps
                  << std::setw(13) << std::setprecision(1) << info.annualized_basis << "%"
                  << "\n";
    }
}

void TerminalUI::print_positions(const std::map<std::string, DeltaNeutralPosition>& positions) {
    std::cout << "\n  OPEN POSITIONS\n";
    print_line('-', 80);

    if (positions.empty()) {
        std::cout << "  (no open positions)\n";
        return;
    }

    std::cout << std::left
              << std::setw(12) << "  Symbol"
              << std::right
              << std::setw(12) << "Spot Size"
              << std::setw(12) << "Fut Size"
              << std::setw(10) << "Delta"
              << std::setw(12) << "Funding $"
              << std::setw(10) << "Periods"
              << std::setw(12) << "Total PnL"
              << "\n";
    print_line('-', 80);

    for (const auto& [symbol, pos] : positions) {
        if (!pos.is_open()) continue;
        std::cout << std::left << "  " << std::setw(10) << symbol
                  << std::right << std::fixed
                  << std::setw(12) << std::setprecision(6) << pos.spot_size
                  << std::setw(12) << std::setprecision(6) << pos.futures_size
                  << std::setw(10) << std::setprecision(6) << pos.net_delta()
                  << std::setw(12) << std::setprecision(4) << pos.funding_collected
                  << std::setw(10) << pos.funding_periods
                  << std::setw(12) << std::setprecision(4) << pos.total_pnl()
                  << "\n";
    }
}

void TerminalUI::print_pnl_summary(double total_pnl, double daily_pnl, double total_funding) {
    std::cout << "\n  P&L SUMMARY\n";
    print_line('-', 80);
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  Total PnL:     $" << total_pnl
              << "  |  Daily PnL:  $" << daily_pnl
              << "  |  Funding:    $" << total_funding << "\n";
}

void TerminalUI::print_events(const std::vector<std::string>& events) {
    std::cout << "\n  RECENT EVENTS\n";
    print_line('-', 80);
    int count = 0;
    for (auto it = events.rbegin(); it != events.rend() && count < 10; ++it, ++count) {
        std::cout << "  " << *it << "\n";
    }
    if (events.empty()) {
        std::cout << "  (no events)\n";
    }
}

void TerminalUI::render(TradingMode mode,
                        const std::map<std::string, FundingInfo>& funding,
                        const std::map<std::string, BasisInfo>& basis,
                        const std::map<std::string, DeltaNeutralPosition>& positions,
                        double total_pnl,
                        double daily_pnl,
                        double total_funding,
                        bool kill_switch_active,
                        const std::vector<std::string>& recent_events)
{
    clear_screen();
    print_header(mode, kill_switch_active);
    print_funding_table(funding);
    print_basis_table(basis);
    print_positions(positions);
    print_pnl_summary(total_pnl, daily_pnl, total_funding);
    print_events(recent_events);
    std::cout << "\n  [q] Quit  [k] Kill switch  [r] Refresh\n";
    std::cout << std::flush;
}

void TerminalUI::log_event(const std::string& event) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    event_log_.push_back(event);
    if (event_log_.size() > 100) event_log_.pop_front();
}

void TerminalUI::check_input() {
    // Non-blocking input check
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};

    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
        char c;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == 'q' || c == 'Q') quit_ = true;
        }
    }
}

} // namespace arb
