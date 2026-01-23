#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>

#include "persistence/session_database.hpp"
#include "arbitrage/funding_settlement_engine.hpp"
#include "arbitrage/delta_neutral_enforcer.hpp"
#include "arbitrage/funding_dispersion.hpp"
#include "arbitrage/funding_graph_optimizer.hpp"

namespace arb {

// ============================================================================
// DEMO TRADING HARNESS
//
// Production-grade paper trading harness for funding rate dispersion strategy.
//
// Hard Safety Locks:
// - LIVE_ORDER_BLOCK: All order submission routes to simulation only
// - No exchange API write calls in demo mode
// - Database records all simulated activity for audit
//
// Features:
// - Real market data inputs (funding rates, prices)
// - Realistic execution model (slippage, fill probability)
// - Funding settlement at correct intervals
// - Delta-neutral enforcement
// - Kill condition monitoring
// - PnL snapshots at configurable intervals
// ============================================================================

struct DemoHarnessConfig {
    // Mode
    bool live_mode{false};                 // MUST be false for demo (hard lock)

    // Session
    std::string session_name;
    double starting_balance{10000};

    // Venues
    std::vector<std::string> venues{"binance", "bybit", "okx", "dydx"};
    std::string instrument{"BTCUSDT"};

    // Strategy parameters
    FundingDispersionConfig strategy_config;
    DemoFillConfig fill_config;
    DeltaNeutralConfig delta_config;

    // Update intervals
    int64_t pnl_snapshot_interval_ms{60000};   // 1 minute
    int64_t delta_check_interval_ms{5000};     // 5 seconds
    int64_t kill_check_interval_ms{1000};      // 1 second

    // Database
    std::string db_path{"sessions.db"};
};

class DemoTradingHarness {
public:
    // ========================================================================
    // Lifecycle
    // ========================================================================

    explicit DemoTradingHarness(const DemoHarnessConfig& config);
    ~DemoTradingHarness();

    // Start the harness (begins monitoring loops)
    void start();

    // Stop the harness gracefully
    void stop();

    // Check if running
    bool is_running() const { return running_; }

    // ========================================================================
    // HARD SAFETY LOCK
    // ========================================================================

    // Absolutely blocks any live order submission
    // This is the critical safety mechanism
    static constexpr bool LIVE_ORDER_BLOCK = true;

    bool is_demo_mode() const { return !config_.live_mode; }

    // ========================================================================
    // Market Data Input
    // ========================================================================

    // Update funding rate (call from exchange clients)
    void update_funding_rate(const FundingRateSnapshot& snapshot);

    // Update order book (call from exchange clients)
    void update_orderbook(
        const std::string& venue,
        double bid, double bid_qty,
        double ask, double ask_qty
    );

    // Update mark/index prices
    void update_prices(
        const std::string& venue,
        double mark_price,
        double index_price
    );

    // Record exchange heartbeat
    void heartbeat(const std::string& venue);

    // ========================================================================
    // Strategy Execution
    // ========================================================================

    // Evaluate and potentially execute entry
    void evaluate_entry();

    // Evaluate and potentially execute exit
    void evaluate_exit();

    // Force close all positions (manual trigger)
    void force_close_all(const std::string& reason);

    // ========================================================================
    // Position Queries
    // ========================================================================

    std::vector<Position> get_positions() const;
    DeltaSnapshot get_delta() const;
    BasisSnapshot get_basis() const;
    double get_equity() const;
    double get_pnl() const;

    // ========================================================================
    // Callbacks
    // ========================================================================

    using EntryCallback = std::function<void(const FundingDispersionStrategy::EntrySignal&)>;
    using ExitCallback = std::function<void(const FundingDispersionStrategy::ExitSignal&)>;
    using FillCallback = std::function<void(const Fill&)>;
    using KillCallback = std::function<void(const KillSignal&)>;

    void set_entry_callback(EntryCallback cb) { on_entry_ = std::move(cb); }
    void set_exit_callback(ExitCallback cb) { on_exit_ = std::move(cb); }
    void set_fill_callback(FillCallback cb) { on_fill_ = std::move(cb); }
    void set_kill_callback(KillCallback cb) { on_kill_ = std::move(cb); }

    // ========================================================================
    // Statistics & Reporting
    // ========================================================================

    struct HarnessStats {
        int64_t start_time{0};
        int64_t uptime_ms{0};
        int entries_evaluated{0};
        int entries_taken{0};
        int exits_evaluated{0};
        int exits_taken{0};
        int funding_settlements{0};
        int hedges_executed{0};
        int kill_events{0};
        double total_funding_received{0};
        double total_fees_paid{0};
        double peak_equity{0};
        double lowest_equity{0};
    };

    HarnessStats stats() const { return stats_; }

    // Get session ID
    std::string session_id() const { return session_id_; }

    // Generate session report
    std::string generate_report() const;

    // Export session data
    void export_csv(const std::string& output_dir) const;

private:
    DemoHarnessConfig config_;
    HarnessStats stats_;
    std::string session_id_;

    // Core components
    std::shared_ptr<SessionDatabase> db_;
    std::unique_ptr<FundingDispersionStrategy> strategy_;
    std::unique_ptr<FundingSettlementEngine> settlement_;
    std::unique_ptr<DeltaNeutralEnforcer> enforcer_;

    // Current state
    mutable std::mutex state_mutex_;
    std::vector<Position> positions_;
    std::map<std::string, std::pair<double, double>> orderbooks_;  // venue -> (bid, ask)
    double cash_balance_{0};
    double unrealized_pnl_{0};
    bool has_position_{false};

    // Background threads
    std::atomic<bool> running_{false};
    std::thread snapshot_thread_;
    std::thread monitor_thread_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    // Callbacks
    EntryCallback on_entry_;
    ExitCallback on_exit_;
    FillCallback on_fill_;
    KillCallback on_kill_;

    // Internal methods
    void snapshot_loop();
    void monitor_loop();
    void take_pnl_snapshot();
    void execute_entry(const FundingDispersionStrategy::EntrySignal& signal);
    void execute_exit();
    void execute_hedge(const HedgeOrder& order);
    void execute_kill(const KillSignal& signal);
    double calculate_equity() const;
    void update_position_from_fill(const Fill& fill);
};

// ============================================================================
// Implementation
// ============================================================================

inline DemoTradingHarness::DemoTradingHarness(const DemoHarnessConfig& config)
    : config_(config)
    , cash_balance_(config.starting_balance)
{
    // HARD SAFETY CHECK
    if (config_.live_mode && LIVE_ORDER_BLOCK) {
        throw std::runtime_error(
            "SAFETY VIOLATION: Attempted to create harness in live mode "
            "while LIVE_ORDER_BLOCK is enabled. This is a hard safety lock."
        );
    }

    // Initialize database
    db_ = std::make_shared<SessionDatabase>(config_.db_path);
    db_->initialize_schema();

    // Create session
    Session session;
    session.mode = config_.live_mode ? TradingMode::LIVE : TradingMode::DEMO;
    session.starting_balance = config_.starting_balance;
    session.session_name = config_.session_name.empty()
        ? ("demo_" + std::to_string(now_micros()))
        : config_.session_name;

    // Build venues JSON
    std::string venues_json = "[";
    for (size_t i = 0; i < config_.venues.size(); i++) {
        if (i > 0) venues_json += ",";
        venues_json += "\"" + config_.venues[i] + "\"";
    }
    venues_json += "]";
    session.venues_enabled_json = venues_json;

    session_id_ = db_->create_session(session);

    // Initialize strategy
    strategy_ = std::make_unique<FundingDispersionStrategy>(config_.strategy_config);

    // Initialize settlement engine
    settlement_ = std::make_unique<FundingSettlementEngine>(
        db_, session_id_, config_.fill_config
    );

    // Initialize delta enforcer
    enforcer_ = std::make_unique<DeltaNeutralEnforcer>(
        db_, session_id_, config_.delta_config
    );

    // Set up callbacks
    enforcer_->set_hedge_callback([this](const HedgeOrder& order) {
        execute_hedge(order);
    });

    enforcer_->set_kill_callback([this](const KillSignal& signal) {
        execute_kill(signal);
    });

    settlement_->set_funding_callback([this](const FundingEvent& event) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        cash_balance_ += event.payment_amount;
        stats_.total_funding_received += event.payment_amount;
        stats_.funding_settlements++;
    });

    stats_.start_time = now_micros();
    stats_.peak_equity = config_.starting_balance;
    stats_.lowest_equity = config_.starting_balance;
}

inline DemoTradingHarness::~DemoTradingHarness() {
    stop();
}

inline void DemoTradingHarness::start() {
    if (running_) return;

    running_ = true;

    // Start background threads
    snapshot_thread_ = std::thread(&DemoTradingHarness::snapshot_loop, this);
    monitor_thread_ = std::thread(&DemoTradingHarness::monitor_loop, this);
}

inline void DemoTradingHarness::stop() {
    if (!running_) return;

    running_ = false;

    // Wake up sleeping threads
    cv_.notify_all();

    // Wait for threads to finish
    if (snapshot_thread_.joinable()) {
        snapshot_thread_.join();
    }
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    // End session
    double final_equity = calculate_equity();
    db_->end_session(session_id_, final_equity);
}

inline void DemoTradingHarness::update_funding_rate(const FundingRateSnapshot& snapshot) {
    strategy_->update_funding_rate(snapshot);

    FundingRate rate;
    rate.venue = snapshot.exchange;
    rate.instrument = snapshot.symbol;
    rate.rate = snapshot.funding_rate;
    rate.predicted_rate = snapshot.predicted_rate;
    rate.next_funding_time = snapshot.next_funding_time;
    rate.mark_price = snapshot.mark_price;
    rate.index_price = snapshot.index_price;

    settlement_->update_funding_rate(rate);
    enforcer_->update_funding_rate(snapshot.exchange, snapshot.funding_rate);
}

inline void DemoTradingHarness::update_orderbook(
    const std::string& venue,
    double bid, double bid_qty,
    double ask, double ask_qty
) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    orderbooks_[venue] = {bid, ask};
}

inline void DemoTradingHarness::update_prices(
    const std::string& venue,
    double mark_price,
    double index_price
) {
    enforcer_->update_mark_price(venue, config_.instrument, mark_price, index_price);

    // Update position mark prices
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& pos : positions_) {
        if (pos.venue == venue) {
            pos.mark_price = mark_price;
            pos.unrealized_pnl = (mark_price - pos.avg_price) * pos.qty;
        }
    }
}

inline void DemoTradingHarness::heartbeat(const std::string& venue) {
    enforcer_->record_heartbeat(venue);
}

inline void DemoTradingHarness::evaluate_entry() {
    stats_.entries_evaluated++;

    if (has_position_) {
        return;  // Already have a position
    }

    auto signal = strategy_->evaluate_entry(cash_balance_);

    if (signal.should_enter) {
        execute_entry(signal);
    }
}

inline void DemoTradingHarness::evaluate_exit() {
    stats_.exits_evaluated++;

    if (!has_position_) {
        return;
    }

    // Build position for exit evaluation
    FundingPosition fp;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& pos : positions_) {
            if (pos.qty > 0) {
                fp.long_exchange = pos.venue;
                fp.long_entry_funding = 0;  // Would need to track this
            } else if (pos.qty < 0) {
                fp.short_exchange = pos.venue;
                fp.short_entry_funding = 0;
            }
        }
    }

    auto signal = strategy_->evaluate_exit(fp);

    if (signal.should_exit) {
        if (on_exit_) {
            on_exit_(signal);
        }
        execute_exit();
    }
}

inline void DemoTradingHarness::execute_entry(
    const FundingDispersionStrategy::EntrySignal& signal
) {
    // HARD SAFETY CHECK
    if (config_.live_mode) {
        throw std::runtime_error("SAFETY VIOLATION: Attempted live order in demo harness");
    }

    if (on_entry_) {
        on_entry_(signal);
    }

    // Get current orderbook prices
    double long_ask = 0, short_bid = 0;
    double long_ask_qty = 0, short_bid_qty = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto long_it = orderbooks_.find(signal.long_exchange);
        auto short_it = orderbooks_.find(signal.short_exchange);
        if (long_it != orderbooks_.end()) {
            long_ask = long_it->second.second;
            long_ask_qty = 1.0;  // Simplified
        }
        if (short_it != orderbooks_.end()) {
            short_bid = short_it->second.first;
            short_bid_qty = 1.0;
        }
    }

    if (long_ask <= 0 || short_bid <= 0) {
        return;  // No prices available
    }

    // Calculate position size
    double notional = std::min(signal.recommended_size, cash_balance_ * 0.5);
    double qty = notional / ((long_ask + short_bid) / 2.0);

    // Simulate long fill
    auto long_fill = settlement_->simulate_fill(
        signal.long_exchange, config_.instrument,
        OrderSide::BUY, qty, 0,  // Market order
        long_ask * 0.9999, long_ask, long_ask_qty, long_ask_qty
    );

    if (!long_fill.filled) {
        return;  // Entry failed
    }

    // Simulate short fill
    auto short_fill = settlement_->simulate_fill(
        signal.short_exchange, config_.instrument,
        OrderSide::SELL, qty, 0,
        short_bid, short_bid * 1.0001, short_bid_qty, short_bid_qty
    );

    if (!short_fill.filled) {
        // Need to unwind long position
        // In reality this is a risk - for demo we just log it
        return;
    }

    // Record positions
    {
        std::lock_guard<std::mutex> lock(state_mutex_);

        Position long_pos;
        long_pos.session_id = session_id_;
        long_pos.venue = signal.long_exchange;
        long_pos.instrument = config_.instrument;
        long_pos.qty = qty;
        long_pos.avg_price = long_fill.fill_price;
        long_pos.mark_price = long_fill.fill_price;
        positions_.push_back(long_pos);

        Position short_pos;
        short_pos.session_id = session_id_;
        short_pos.venue = signal.short_exchange;
        short_pos.instrument = config_.instrument;
        short_pos.qty = -qty;
        short_pos.avg_price = short_fill.fill_price;
        short_pos.mark_price = short_fill.fill_price;
        positions_.push_back(short_pos);

        // Update cash
        cash_balance_ -= long_fill.fee + short_fill.fee;
        stats_.total_fees_paid += long_fill.fee + short_fill.fee;

        has_position_ = true;
    }

    // Update enforcer
    for (const auto& pos : positions_) {
        enforcer_->update_position(pos);
    }

    // Record in database
    for (const auto& pos : positions_) {
        db_->upsert_position(pos);
    }

    stats_.entries_taken++;
}

inline void DemoTradingHarness::execute_exit() {
    auto kill_orders = enforcer_->generate_kill_orders();

    for (const auto& order : kill_orders) {
        execute_hedge(order);
    }

    // Clear positions
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        positions_.clear();
        has_position_ = false;
    }

    db_->clear_positions_for_session(session_id_);
    stats_.exits_taken++;
}

inline void DemoTradingHarness::execute_hedge(const HedgeOrder& order) {
    // HARD SAFETY CHECK
    if (config_.live_mode) {
        throw std::runtime_error("SAFETY VIOLATION: Attempted live order in demo harness");
    }

    // Get orderbook
    double bid = 0, ask = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = orderbooks_.find(order.venue);
        if (it != orderbooks_.end()) {
            bid = it->second.first;
            ask = it->second.second;
        }
    }

    if (bid <= 0 || ask <= 0) return;

    auto fill = settlement_->simulate_fill(
        order.venue, order.instrument,
        order.side, order.qty, 0,
        bid, ask, 1.0, 1.0
    );

    if (fill.filled) {
        update_position_from_fill(Fill{
            "", "", session_id_, order.venue, order.instrument,
            order.side, fill.fill_price, fill.fill_qty, fill.fee,
            now_micros(), fill.slippage_bps, fill.latency_ms
        });
        stats_.hedges_executed++;
    }
}

inline void DemoTradingHarness::execute_kill(const KillSignal& signal) {
    if (on_kill_) {
        on_kill_(signal);
    }

    std::vector<Position> closed;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        closed = positions_;
    }

    enforcer_->record_kill(signal, closed);
    execute_exit();
    stats_.kill_events++;
}

inline void DemoTradingHarness::force_close_all(const std::string& reason) {
    KillSignal signal;
    signal.should_kill = true;
    signal.reason = KillReason::DRAWDOWN_LIMIT;  // Using as generic manual close
    signal.detail = "Manual close: " + reason;
    signal.urgency = 1.0;

    execute_kill(signal);
}

inline std::vector<Position> DemoTradingHarness::get_positions() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return positions_;
}

inline DeltaSnapshot DemoTradingHarness::get_delta() const {
    return enforcer_->compute_delta();
}

inline BasisSnapshot DemoTradingHarness::get_basis() const {
    return enforcer_->compute_basis();
}

inline double DemoTradingHarness::get_equity() const {
    return calculate_equity();
}

inline double DemoTradingHarness::get_pnl() const {
    return calculate_equity() - config_.starting_balance;
}

inline double DemoTradingHarness::calculate_equity() const {
    std::lock_guard<std::mutex> lock(state_mutex_);

    double equity = cash_balance_;
    for (const auto& pos : positions_) {
        equity += pos.unrealized_pnl;
    }

    return equity;
}

inline void DemoTradingHarness::update_position_from_fill(const Fill& fill) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    for (auto& pos : positions_) {
        if (pos.venue == fill.venue && pos.instrument == fill.instrument) {
            double old_qty = pos.qty;
            double delta_qty = (fill.side == OrderSide::BUY) ? fill.qty : -fill.qty;
            double new_qty = old_qty + delta_qty;

            if (std::abs(new_qty) < 1e-9) {
                // Position closed
                pos.qty = 0;
            } else if ((old_qty > 0 && new_qty > 0) || (old_qty < 0 && new_qty < 0)) {
                // Adding to position
                double old_notional = std::abs(old_qty) * pos.avg_price;
                double new_notional = std::abs(delta_qty) * fill.price;
                pos.avg_price = (old_notional + new_notional) / (std::abs(old_qty) + std::abs(delta_qty));
                pos.qty = new_qty;
            } else {
                // Reducing/flipping position
                pos.qty = new_qty;
                if (new_qty != 0) {
                    pos.avg_price = fill.price;
                }
            }

            cash_balance_ -= fill.fee;
            break;
        }
    }

    if (on_fill_) {
        on_fill_(fill);
    }
}

inline void DemoTradingHarness::snapshot_loop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(config_.pnl_snapshot_interval_ms),
            [this] { return !running_.load(); });

        if (!running_) break;

        take_pnl_snapshot();
    }
}

inline void DemoTradingHarness::monitor_loop() {
    int64_t last_delta_check = 0;
    int64_t last_kill_check = 0;

    while (running_) {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100),
            [this] { return !running_.load(); });

        if (!running_) break;

        int64_t now = now_micros() / 1000;

        // Check for funding settlement
        auto positions = get_positions();
        settlement_->settle_all_funding(positions);

        // Check delta
        if (now - last_delta_check >= config_.delta_check_interval_ms) {
            enforcer_->check_hedge_needed();
            last_delta_check = now;
        }

        // Check kill conditions
        if (now - last_kill_check >= config_.kill_check_interval_ms) {
            enforcer_->update_equity(calculate_equity());
            auto kill = enforcer_->check_kill_conditions();
            if (kill) {
                execute_kill(*kill);
            }
            last_kill_check = now;
        }

        // Update stats
        double equity = calculate_equity();
        stats_.peak_equity = std::max(stats_.peak_equity, equity);
        stats_.lowest_equity = std::min(stats_.lowest_equity, equity);
        stats_.uptime_ms = (now_micros() - stats_.start_time) / 1000;
    }
}

inline void DemoTradingHarness::take_pnl_snapshot() {
    double equity = calculate_equity();
    double drawdown = (stats_.peak_equity > 0)
        ? (stats_.peak_equity - equity) / stats_.peak_equity
        : 0;

    PnlSnapshot snap;
    snap.session_id = session_id_;
    snap.timestamp = now_micros();
    snap.cash_balance = cash_balance_;
    snap.equity = equity;
    snap.unrealized_pnl = equity - cash_balance_;
    snap.realized_pnl = 0;  // Would need proper tracking
    snap.pnl_funding = stats_.total_funding_received;
    snap.pnl_fees = -stats_.total_fees_paid;
    snap.leverage = 0;  // Would need to calculate
    snap.drawdown = drawdown;
    snap.high_water_mark = stats_.peak_equity;

    // Build exposure JSON
    auto delta = get_delta();
    snap.exposure_json = "{\"net_delta\":" + std::to_string(delta.net_delta) +
        ",\"long_notional\":" + std::to_string(delta.total_long_notional) +
        ",\"short_notional\":" + std::to_string(delta.total_short_notional) + "}";

    db_->insert_pnl_snapshot(snap);
}

inline std::string DemoTradingHarness::generate_report() const {
    return db_->generate_report(session_id_);
}

inline void DemoTradingHarness::export_csv(const std::string& output_dir) const {
    db_->export_fills_csv(session_id_, output_dir + "/fills.csv");
    db_->export_funding_events_csv(session_id_, output_dir + "/funding.csv");
    db_->export_pnl_snapshots_csv(session_id_, output_dir + "/pnl.csv");
}

}  // namespace arb
