#pragma once

#include "common/types.hpp"
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <mutex>
#include <atomic>

namespace arb {

class TerminalUI {
public:
    TerminalUI();

    void render(TradingMode mode,
                const std::map<std::string, FundingInfo>& funding,
                const std::map<std::string, BasisInfo>& basis,
                const std::map<std::string, DeltaNeutralPosition>& positions,
                double total_pnl,
                double daily_pnl,
                double total_funding,
                bool kill_switch_active,
                const std::vector<std::string>& recent_events);

    void log_event(const std::string& event);
    bool should_quit() const { return quit_.load(); }
    void check_input();

private:
    void clear_screen();
    void print_header(TradingMode mode, bool kill_switch_active);
    void print_funding_table(const std::map<std::string, FundingInfo>& funding);
    void print_basis_table(const std::map<std::string, BasisInfo>& basis);
    void print_positions(const std::map<std::string, DeltaNeutralPosition>& positions);
    void print_pnl_summary(double total_pnl, double daily_pnl, double total_funding);
    void print_events(const std::vector<std::string>& events);
    void print_line(char c = '-', int width = 80);

    std::atomic<bool> quit_{false};
    std::deque<std::string> event_log_;
    std::mutex log_mutex_;
};

} // namespace arb
