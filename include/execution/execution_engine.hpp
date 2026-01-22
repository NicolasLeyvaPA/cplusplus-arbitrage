#pragma once

#include <memory>
#include <mutex>
#include <map>
#include <queue>
#include <functional>
#include <thread>
#include <atomic>
#include <condition_variable>
#include "common/types.hpp"
#include "config/config.hpp"
#include "execution/order.hpp"
#include "risk/risk_manager.hpp"
#include "market_data/polymarket_client.hpp"

namespace arb {

/**
 * Execution engine handles order lifecycle management.
 * Supports dry-run, paper, and live modes.
 */
class ExecutionEngine {
public:
    using FillCallback = std::function<void(const Fill&)>;
    using OrderCallback = std::function<void(const Order&)>;

    ExecutionEngine(
        TradingMode mode,
        std::shared_ptr<RiskManager> risk_manager,
        std::shared_ptr<PolymarketClient> polymarket_client
    );
    ~ExecutionEngine();

    // Submit order
    struct SubmitResult {
        bool accepted{false};
        std::string order_id;
        std::string rejection_reason;
    };

    SubmitResult submit_order(const Signal& signal);
    SubmitResult submit_paired_order(const Signal& yes_signal, const Signal& no_signal);

    // Order management
    bool cancel_order(const std::string& order_id);
    bool cancel_all();

    // Query orders
    std::optional<Order> get_order(const std::string& order_id) const;
    std::vector<Order> get_open_orders() const;
    std::vector<Order> get_orders_for_market(const std::string& market_id) const;

    // Callbacks
    void set_fill_callback(FillCallback cb) { on_fill_ = std::move(cb); }
    void set_order_callback(OrderCallback cb) { on_order_update_ = std::move(cb); }

    // Stats
    int64_t orders_submitted() const { return orders_submitted_.load(); }
    int64_t orders_filled() const { return orders_filled_.load(); }
    int64_t orders_rejected() const { return orders_rejected_.load(); }

    // Latency metrics
    LatencyMetrics get_latency_metrics() const;

    // Mode
    TradingMode mode() const { return mode_; }

private:
    TradingMode mode_;
    std::shared_ptr<RiskManager> risk_manager_;
    std::shared_ptr<PolymarketClient> polymarket_client_;

    FillCallback on_fill_;
    OrderCallback on_order_update_;

    // Order storage
    mutable std::mutex orders_mutex_;
    std::map<std::string, Order> orders_;
    std::map<std::string, PairedOrder> paired_orders_;

    // Stats
    std::atomic<int64_t> orders_submitted_{0};
    std::atomic<int64_t> orders_filled_{0};
    std::atomic<int64_t> orders_rejected_{0};

    // Latency tracking
    mutable std::mutex latency_mutex_;
    std::vector<Duration> decision_to_send_times_;
    std::vector<Duration> send_to_ack_times_;

    // Paper trading simulation
    void simulate_fill(Order& order);
    void process_paper_order(const std::string& order_id);

    // Live order management
    void send_live_order(Order& order);
    void handle_order_response(const std::string& order_id,
                               const PolymarketClient::OrderResponse& response);

    // Order state transitions
    void update_order_state(const std::string& order_id, OrderState new_state);
    void record_fill(const std::string& order_id, const Fill& fill);

    // Paired order handling
    void process_paired_order(PairedOrder& pair);
    void check_unwind_needed(PairedOrder& pair);

    // Worker thread for paper simulation
    std::atomic<bool> running_{true};
    std::thread worker_thread_;
    std::queue<std::string> pending_paper_orders_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    void paper_simulation_loop();
};

} // namespace arb
