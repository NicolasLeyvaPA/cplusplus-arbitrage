#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <string>
#include <functional>
#include "common/types.hpp"
#include "config/config.hpp"

namespace arb {

// Forward declarations
class BinanceClient;
class PolymarketClient;
class PositionManager;
class RiskManager;
class ExecutionEngine;
class BinaryMarketBook;

/**
 * Terminal UI for displaying bot status and activity.
 * Works with or without ncurses (falls back to simple text mode).
 */
class TerminalUI {
public:
    TerminalUI(
        TradingMode mode,
        std::shared_ptr<BinanceClient> binance,
        std::shared_ptr<PolymarketClient> polymarket,
        std::shared_ptr<PositionManager> positions,
        std::shared_ptr<RiskManager> risk,
        std::shared_ptr<ExecutionEngine> execution
    );
    ~TerminalUI();

    // Start/stop the UI refresh loop
    void start();
    void stop();

    // Add events to the activity log
    void log_trade(const Fill& fill);
    void log_signal(const Signal& signal);
    void log_order(const std::string& order_id, const std::string& status);
    void log_error(const std::string& error);
    void log_info(const std::string& info);

    // Set current market being displayed
    void set_active_market(const std::string& market_id);

    // User input handling
    using CommandCallback = std::function<void(const std::string&)>;
    void set_command_callback(CommandCallback cb) { on_command_ = std::move(cb); }

    // Configuration
    void set_refresh_rate_ms(int ms) { refresh_rate_ms_ = ms; }

private:
    TradingMode mode_;
    std::shared_ptr<BinanceClient> binance_;
    std::shared_ptr<PolymarketClient> polymarket_;
    std::shared_ptr<PositionManager> positions_;
    std::shared_ptr<RiskManager> risk_;
    std::shared_ptr<ExecutionEngine> execution_;

    CommandCallback on_command_;

    std::atomic<bool> running_{false};
    std::thread ui_thread_;
    int refresh_rate_ms_{100};

    // Activity log
    struct LogEntry {
        WallClock timestamp;
        std::string type;     // "TRADE", "SIGNAL", "ORDER", "ERROR", "INFO"
        std::string message;
    };
    std::deque<LogEntry> activity_log_;
    std::mutex log_mutex_;
    static constexpr size_t MAX_LOG_ENTRIES = 100;

    // Active market for display
    std::string active_market_id_;
    std::mutex market_mutex_;

#ifdef HAS_NCURSES
    // ncurses specific
    void* header_win_{nullptr};
    void* market_win_{nullptr};
    void* orders_win_{nullptr};
    void* activity_win_{nullptr};
    void* input_win_{nullptr};

    void init_ncurses();
    void cleanup_ncurses();
    void draw_ncurses();
    void draw_header();
    void draw_market_panel();
    void draw_orders_panel();
    void draw_activity_panel();
    void handle_input();
#endif

    // Text mode (fallback)
    void draw_text_mode();
    void print_separator(char c = '-', int width = 80);
    void print_centered(const std::string& text, int width = 80);

    // UI refresh loop
    void refresh_loop();

    // Formatting helpers
    std::string format_price(Price price) const;
    std::string format_size(Size size) const;
    std::string format_pnl(Notional pnl) const;
    std::string format_duration(Duration d) const;
    std::string format_timestamp(WallClock t) const;
    std::string connection_status_string(ConnectionStatus s) const;
    std::string colorize(const std::string& text, const std::string& color) const;
};

} // namespace arb
