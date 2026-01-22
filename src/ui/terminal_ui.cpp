#include "ui/terminal_ui.hpp"
#include "market_data/binance_client.hpp"
#include "market_data/polymarket_client.hpp"
#include "market_data/order_book.hpp"
#include "position/position_manager.hpp"
#include "risk/risk_manager.hpp"
#include "execution/execution_engine.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <iostream>
#include <iomanip>
#include <ctime>

#ifdef HAS_NCURSES
#include <ncurses.h>
#endif

namespace arb {

TerminalUI::TerminalUI(
    TradingMode mode,
    std::shared_ptr<BinanceClient> binance,
    std::shared_ptr<PolymarketClient> polymarket,
    std::shared_ptr<PositionManager> positions,
    std::shared_ptr<RiskManager> risk,
    std::shared_ptr<ExecutionEngine> execution)
    : mode_(mode)
    , binance_(std::move(binance))
    , polymarket_(std::move(polymarket))
    , positions_(std::move(positions))
    , risk_(std::move(risk))
    , execution_(std::move(execution))
{
}

TerminalUI::~TerminalUI() {
    stop();
}

void TerminalUI::start() {
    if (running_.load()) return;

    running_ = true;

#ifdef HAS_NCURSES
    init_ncurses();
#endif

    ui_thread_ = std::thread(&TerminalUI::refresh_loop, this);
}

void TerminalUI::stop() {
    running_ = false;
    if (ui_thread_.joinable()) {
        ui_thread_.join();
    }

#ifdef HAS_NCURSES
    cleanup_ncurses();
#endif
}

void TerminalUI::log_trade(const Fill& fill) {
    std::lock_guard<std::mutex> lock(log_mutex_);

    LogEntry entry;
    entry.timestamp = wall_now();
    entry.type = "TRADE";
    entry.message = fmt::format("{} {} {:.2f} @ {:.4f} (${:.2f})",
                               side_to_string(fill.side),
                               fill.token_id.substr(0, 8),
                               fill.size, fill.price, fill.notional);

    activity_log_.push_front(entry);
    while (activity_log_.size() > MAX_LOG_ENTRIES) {
        activity_log_.pop_back();
    }
}

void TerminalUI::log_signal(const Signal& signal) {
    std::lock_guard<std::mutex> lock(log_mutex_);

    LogEntry entry;
    entry.timestamp = wall_now();
    entry.type = "SIGNAL";
    entry.message = fmt::format("[{}] {} {} edge={:.2f}c conf={:.2f}",
                               signal.strategy_name,
                               side_to_string(signal.side),
                               signal.token_id.substr(0, 8),
                               signal.expected_edge,
                               signal.confidence);

    activity_log_.push_front(entry);
    while (activity_log_.size() > MAX_LOG_ENTRIES) {
        activity_log_.pop_back();
    }
}

void TerminalUI::log_order(const std::string& order_id, const std::string& status) {
    std::lock_guard<std::mutex> lock(log_mutex_);

    LogEntry entry;
    entry.timestamp = wall_now();
    entry.type = "ORDER";
    entry.message = fmt::format("{}: {}", order_id.substr(0, 12), status);

    activity_log_.push_front(entry);
    while (activity_log_.size() > MAX_LOG_ENTRIES) {
        activity_log_.pop_back();
    }
}

void TerminalUI::log_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(log_mutex_);

    LogEntry entry;
    entry.timestamp = wall_now();
    entry.type = "ERROR";
    entry.message = error;

    activity_log_.push_front(entry);
    while (activity_log_.size() > MAX_LOG_ENTRIES) {
        activity_log_.pop_back();
    }
}

void TerminalUI::log_info(const std::string& info) {
    std::lock_guard<std::mutex> lock(log_mutex_);

    LogEntry entry;
    entry.timestamp = wall_now();
    entry.type = "INFO";
    entry.message = info;

    activity_log_.push_front(entry);
    while (activity_log_.size() > MAX_LOG_ENTRIES) {
        activity_log_.pop_back();
    }
}

void TerminalUI::set_active_market(const std::string& market_id) {
    std::lock_guard<std::mutex> lock(market_mutex_);
    active_market_id_ = market_id;
}

void TerminalUI::refresh_loop() {
    while (running_.load()) {
#ifdef HAS_NCURSES
        draw_ncurses();
        handle_input();
#else
        draw_text_mode();
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(refresh_rate_ms_));
    }
}

std::string TerminalUI::format_price(Price price) const {
    return fmt::format("{:.4f}", price);
}

std::string TerminalUI::format_size(Size size) const {
    return fmt::format("{:.2f}", size);
}

std::string TerminalUI::format_pnl(Notional pnl) const {
    if (pnl >= 0) {
        return fmt::format("+${:.2f}", pnl);
    }
    return fmt::format("-${:.2f}", -pnl);
}

std::string TerminalUI::format_duration(Duration d) const {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    if (ms < 1000) {
        return fmt::format("{}ms", ms);
    }
    return fmt::format("{:.1f}s", ms / 1000.0);
}

std::string TerminalUI::format_timestamp(WallClock t) const {
    auto time_t = std::chrono::system_clock::to_time_t(t);
    std::tm tm = *std::localtime(&time_t);
    return fmt::format("{:02d}:{:02d}:{:02d}", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string TerminalUI::connection_status_string(ConnectionStatus s) const {
    switch (s) {
        case ConnectionStatus::CONNECTED: return "CONN";
        case ConnectionStatus::CONNECTING: return "WAIT";
        case ConnectionStatus::DISCONNECTED: return "DOWN";
        case ConnectionStatus::RECONNECTING: return "RECON";
        case ConnectionStatus::ERROR: return "ERR!";
    }
    return "????";
}

void TerminalUI::print_separator(char c, int width) {
    std::cout << std::string(width, c) << "\n";
}

void TerminalUI::print_centered(const std::string& text, int width) {
    int padding = (width - static_cast<int>(text.length())) / 2;
    std::cout << std::string(std::max(0, padding), ' ') << text << "\n";
}

void TerminalUI::draw_text_mode() {
    // Clear screen
    std::cout << "\033[2J\033[H";

    int width = 80;

    // Header
    print_separator('=', width);
    std::string title = fmt::format("DAILYARB - {} MODE", mode_to_string(mode_));
    print_centered(title, width);
    print_separator('=', width);

    // Connection status
    std::cout << "\n";
    std::cout << fmt::format("  Binance:    [{}]  msgs: {}\n",
                            connection_status_string(binance_ ? binance_->status() : ConnectionStatus::DISCONNECTED),
                            binance_ ? binance_->messages_received() : 0);
    std::cout << fmt::format("  Polymarket: [{}]  msgs: {}\n",
                            connection_status_string(polymarket_ ? polymarket_->status() : ConnectionStatus::DISCONNECTED),
                            polymarket_ ? polymarket_->messages_received() : 0);

    // BTC Price
    if (binance_) {
        auto btc = binance_->current_price();
        std::cout << fmt::format("\n  BTC/USDT: ${:.2f} (bid: {:.2f} ask: {:.2f})\n",
                                btc.mid, btc.bid, btc.ask);
    }

    print_separator('-', width);

    // Active market book
    {
        std::lock_guard<std::mutex> lock(market_mutex_);
        if (!active_market_id_.empty() && polymarket_) {
            auto* book = polymarket_->get_market_book(active_market_id_);
            if (book && book->has_liquidity()) {
                std::cout << fmt::format("\n  Market: {}\n", active_market_id_);

                auto yes_bid = book->yes_book().best_bid();
                auto yes_ask = book->yes_book().best_ask();
                auto no_bid = book->no_book().best_bid();
                auto no_ask = book->no_book().best_ask();

                std::cout << fmt::format("  YES: bid {:.4f} ({:.1f}) | ask {:.4f} ({:.1f})\n",
                                        yes_bid ? yes_bid->price : 0.0, yes_bid ? yes_bid->size : 0.0,
                                        yes_ask ? yes_ask->price : 0.0, yes_ask ? yes_ask->size : 0.0);
                std::cout << fmt::format("  NO:  bid {:.4f} ({:.1f}) | ask {:.4f} ({:.1f})\n",
                                        no_bid ? no_bid->price : 0.0, no_bid ? no_bid->size : 0.0,
                                        no_ask ? no_ask->price : 0.0, no_ask ? no_ask->size : 0.0);

                double sum_asks = book->sum_of_best_asks();
                std::cout << fmt::format("  Sum of asks: {:.4f} (edge if < 0.98: {:.2f}c)\n",
                                        sum_asks, (0.98 - sum_asks) * 100);
            }
        }
    }

    print_separator('-', width);

    // Risk & PnL
    std::cout << "\n  RISK STATUS\n";

    if (risk_) {
        std::cout << fmt::format("  Daily PnL: {}  |  Exposure: ${:.2f}  |  Positions: {}\n",
                                format_pnl(risk_->daily_pnl()),
                                risk_->current_exposure(),
                                risk_->open_position_count());

        if (risk_->is_kill_switch_active()) {
            std::cout << "  *** KILL SWITCH ACTIVE: " << risk_->kill_switch_reason() << " ***\n";
        }
    }

    if (positions_) {
        std::cout << fmt::format("  Total PnL: {}  |  Fees: ${:.2f}\n",
                                format_pnl(positions_->total_pnl()),
                                positions_->total_fees());
    }

    print_separator('-', width);

    // Open orders
    std::cout << "\n  OPEN ORDERS\n";
    if (execution_) {
        auto orders = execution_->get_open_orders();
        if (orders.empty()) {
            std::cout << "  (none)\n";
        } else {
            for (const auto& order : orders) {
                std::cout << fmt::format("  {} {} {} @ {} ({})\n",
                                        order.client_order_id.substr(0, 12),
                                        side_to_string(order.side),
                                        format_size(order.remaining_size),
                                        format_price(order.price),
                                        order_state_to_string(order.state));
            }
        }
    }

    print_separator('-', width);

    // Activity log
    std::cout << "\n  ACTIVITY LOG\n";
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        int shown = 0;
        for (const auto& entry : activity_log_) {
            if (shown >= 10) break;

            std::string type_str;
            if (entry.type == "TRADE") type_str = "[TRD]";
            else if (entry.type == "SIGNAL") type_str = "[SIG]";
            else if (entry.type == "ORDER") type_str = "[ORD]";
            else if (entry.type == "ERROR") type_str = "[ERR]";
            else type_str = "[INF]";

            std::cout << fmt::format("  {} {} {}\n",
                                    format_timestamp(entry.timestamp),
                                    type_str,
                                    entry.message);
            shown++;
        }
    }

    print_separator('=', width);
    std::cout << "  Press Ctrl+C to exit\n";
    std::cout.flush();
}

#ifdef HAS_NCURSES

void TerminalUI::init_ncurses() {
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_GREEN, COLOR_BLACK);
        init_pair(2, COLOR_RED, COLOR_BLACK);
        init_pair(3, COLOR_YELLOW, COLOR_BLACK);
        init_pair(4, COLOR_CYAN, COLOR_BLACK);
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    // Create windows
    header_win_ = newwin(3, max_x, 0, 0);
    market_win_ = newwin(10, max_x, 3, 0);
    orders_win_ = newwin(8, max_x, 13, 0);
    activity_win_ = newwin(max_y - 23, max_x, 21, 0);
    input_win_ = newwin(2, max_x, max_y - 2, 0);
}

void TerminalUI::cleanup_ncurses() {
    if (header_win_) delwin(static_cast<WINDOW*>(header_win_));
    if (market_win_) delwin(static_cast<WINDOW*>(market_win_));
    if (orders_win_) delwin(static_cast<WINDOW*>(orders_win_));
    if (activity_win_) delwin(static_cast<WINDOW*>(activity_win_));
    if (input_win_) delwin(static_cast<WINDOW*>(input_win_));
    endwin();
}

void TerminalUI::draw_ncurses() {
    draw_header();
    draw_market_panel();
    draw_orders_panel();
    draw_activity_panel();

    doupdate();
}

void TerminalUI::draw_header() {
    WINDOW* win = static_cast<WINDOW*>(header_win_);
    werase(win);
    box(win, 0, 0);

    std::string title = fmt::format(" DAILYARB - {} ", mode_to_string(mode_));
    mvwprintw(win, 0, 2, "%s", title.c_str());

    // Connection status
    std::string binance_status = fmt::format("Binance:[{}]",
        connection_status_string(binance_ ? binance_->status() : ConnectionStatus::DISCONNECTED));
    std::string poly_status = fmt::format("Polymarket:[{}]",
        connection_status_string(polymarket_ ? polymarket_->status() : ConnectionStatus::DISCONNECTED));

    int attr = (binance_ && binance_->is_connected()) ? COLOR_PAIR(1) : COLOR_PAIR(2);
    wattron(win, attr);
    mvwprintw(win, 1, 2, "%s", binance_status.c_str());
    wattroff(win, attr);

    attr = (polymarket_ && polymarket_->is_connected()) ? COLOR_PAIR(1) : COLOR_PAIR(2);
    wattron(win, attr);
    mvwprintw(win, 1, 25, "%s", poly_status.c_str());
    wattroff(win, attr);

    // BTC price
    if (binance_) {
        auto btc = binance_->current_price();
        mvwprintw(win, 1, 50, "BTC: $%.2f", btc.mid);
    }

    wnoutrefresh(win);
}

void TerminalUI::draw_market_panel() {
    WINDOW* win = static_cast<WINDOW*>(market_win_);
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " Market Book ");

    std::lock_guard<std::mutex> lock(market_mutex_);
    if (!active_market_id_.empty() && polymarket_) {
        auto* book = polymarket_->get_market_book(active_market_id_);
        if (book) {
            mvwprintw(win, 1, 2, "Market: %s", active_market_id_.c_str());

            auto yes_bid = book->yes_book().best_bid();
            auto yes_ask = book->yes_book().best_ask();
            auto no_bid = book->no_book().best_bid();
            auto no_ask = book->no_book().best_ask();

            mvwprintw(win, 3, 2, "YES: bid %.4f | ask %.4f",
                     yes_bid ? yes_bid->price : 0.0,
                     yes_ask ? yes_ask->price : 0.0);
            mvwprintw(win, 4, 2, "NO:  bid %.4f | ask %.4f",
                     no_bid ? no_bid->price : 0.0,
                     no_ask ? no_ask->price : 0.0);

            double sum = book->sum_of_best_asks();
            mvwprintw(win, 6, 2, "Sum of asks: %.4f", sum);

            if (sum < 0.98) {
                wattron(win, COLOR_PAIR(1) | A_BOLD);
                mvwprintw(win, 7, 2, "EDGE: %.2f cents", (0.98 - sum) * 100);
                wattroff(win, COLOR_PAIR(1) | A_BOLD);
            }
        }
    }

    wnoutrefresh(win);
}

void TerminalUI::draw_orders_panel() {
    WINDOW* win = static_cast<WINDOW*>(orders_win_);
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " Orders & Risk ");

    if (risk_) {
        mvwprintw(win, 1, 2, "Daily PnL: %s  Exposure: $%.2f  Positions: %d",
                 format_pnl(risk_->daily_pnl()).c_str(),
                 risk_->current_exposure(),
                 risk_->open_position_count());

        if (risk_->is_kill_switch_active()) {
            wattron(win, COLOR_PAIR(2) | A_BOLD);
            mvwprintw(win, 2, 2, "KILL SWITCH: %s", risk_->kill_switch_reason().c_str());
            wattroff(win, COLOR_PAIR(2) | A_BOLD);
        }
    }

    if (execution_) {
        auto orders = execution_->get_open_orders();
        int row = 4;
        for (const auto& order : orders) {
            if (row >= 7) break;
            mvwprintw(win, row, 2, "%s %s %.2f @ %.4f",
                     order.client_order_id.substr(0, 10).c_str(),
                     side_to_string(order.side).c_str(),
                     order.remaining_size,
                     order.price);
            row++;
        }
    }

    wnoutrefresh(win);
}

void TerminalUI::draw_activity_panel() {
    WINDOW* win = static_cast<WINDOW*>(activity_win_);
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " Activity ");

    std::lock_guard<std::mutex> lock(log_mutex_);
    int row = 1;
    int max_row;
    int max_col;
    getmaxyx(win, max_row, max_col);

    for (const auto& entry : activity_log_) {
        if (row >= max_row - 1) break;

        int attr = 0;
        if (entry.type == "TRADE") attr = COLOR_PAIR(1);
        else if (entry.type == "ERROR") attr = COLOR_PAIR(2);
        else if (entry.type == "SIGNAL") attr = COLOR_PAIR(3);

        if (attr) wattron(win, attr);
        mvwprintw(win, row, 2, "%s [%s] %s",
                 format_timestamp(entry.timestamp).c_str(),
                 entry.type.c_str(),
                 entry.message.substr(0, max_col - 20).c_str());
        if (attr) wattroff(win, attr);

        row++;
    }

    wnoutrefresh(win);
}

void TerminalUI::handle_input() {
    int ch = getch();
    if (ch == ERR) return;

    switch (ch) {
        case 'q':
        case 'Q':
            running_ = false;
            break;
        case 'k':
        case 'K':
            if (risk_) {
                if (risk_->is_kill_switch_active()) {
                    risk_->deactivate_kill_switch();
                } else {
                    risk_->activate_kill_switch("Manual activation");
                }
            }
            break;
        case 'c':
        case 'C':
            if (execution_) {
                execution_->cancel_all();
            }
            break;
    }
}

#endif  // HAS_NCURSES

} // namespace arb
