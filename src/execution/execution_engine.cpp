#include "execution/execution_engine.hpp"
#include "utils/metrics.hpp"
#include <spdlog/spdlog.h>
#include <random>

namespace arb {

ExecutionEngine::ExecutionEngine(
    TradingMode mode,
    std::shared_ptr<RiskManager> risk_manager,
    std::shared_ptr<PolymarketClient> polymarket_client)
    : mode_(mode)
    , risk_manager_(std::move(risk_manager))
    , polymarket_client_(std::move(polymarket_client))
{
    spdlog::info("ExecutionEngine initialized in {} mode", mode_to_string(mode));

    // Start paper simulation worker if in paper mode
    if (mode_ == TradingMode::PAPER) {
        worker_thread_ = std::thread(&ExecutionEngine::paper_simulation_loop, this);
    }
}

ExecutionEngine::~ExecutionEngine() {
    running_ = false;
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

ExecutionEngine::SubmitResult ExecutionEngine::submit_order(const Signal& signal) {
    SubmitResult result;

    // Calculate notional
    Notional notional = signal.target_price * signal.target_size;

    // Risk check
    auto risk_check = risk_manager_->check_order(signal, notional);
    if (!risk_check.allowed) {
        result.rejection_reason = risk_check.reason;
        orders_rejected_++;
        spdlog::warn("Order rejected: {}", risk_check.reason);
        return result;
    }

    // Rate limit check
    if (!risk_manager_->can_place_order()) {
        result.rejection_reason = "Rate limit exceeded";
        orders_rejected_++;
        return result;
    }

    // Create order
    Order order;
    order.client_order_id = generate_order_id();
    order.strategy_name = signal.strategy_name;
    order.market_id = signal.market_id;
    order.token_id = signal.token_id;
    order.side = signal.side;
    order.type = OrderType::LIMIT;
    order.price = signal.target_price;
    order.original_size = signal.target_size;
    order.remaining_size = signal.target_size;
    order.created_at = now();

    // Store order
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        orders_[order.client_order_id] = order;
    }

    // Execute based on mode
    switch (mode_) {
        case TradingMode::DRY_RUN:
            spdlog::info("[DRY-RUN] Would place order: {} {} {} @ {:.4f} x {:.2f}",
                        order.client_order_id, side_to_string(order.side),
                        order.token_id, order.price, order.original_size);
            // Simulate immediate acknowledgment for dry-run
            update_order_state(order.client_order_id, OrderState::ACKNOWLEDGED);
            break;

        case TradingMode::PAPER:
            order.mark_sent();
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                pending_paper_orders_.push(order.client_order_id);
            }
            queue_cv_.notify_one();
            spdlog::info("[PAPER] Order submitted: {} {} @ {:.4f}",
                        order.client_order_id, side_to_string(order.side), order.price);
            break;

        case TradingMode::LIVE:
            send_live_order(order);
            break;
    }

    risk_manager_->record_order_placed();
    orders_submitted_++;

    result.accepted = true;
    result.order_id = order.client_order_id;

    if (on_order_update_) {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        on_order_update_(orders_[order.client_order_id]);
    }

    return result;
}

ExecutionEngine::SubmitResult ExecutionEngine::submit_paired_order(
    const Signal& yes_signal, const Signal& no_signal)
{
    SubmitResult result;

    // Combined notional
    Notional total_notional = (yes_signal.target_price * yes_signal.target_size) +
                              (no_signal.target_price * no_signal.target_size);

    // Risk check for combined order
    auto risk_check = risk_manager_->check_order(yes_signal, total_notional);
    if (!risk_check.allowed) {
        result.rejection_reason = risk_check.reason;
        orders_rejected_++;
        return result;
    }

    // Create paired order
    PairedOrder pair;
    pair.pair_id = generate_order_id();
    pair.created_at = now();

    // YES leg
    pair.yes_order.client_order_id = generate_order_id();
    pair.yes_order.strategy_name = yes_signal.strategy_name;
    pair.yes_order.market_id = yes_signal.market_id;
    pair.yes_order.token_id = yes_signal.token_id;
    pair.yes_order.side = yes_signal.side;
    pair.yes_order.type = OrderType::IOC;  // Use IOC for paired orders
    pair.yes_order.price = yes_signal.target_price;
    pair.yes_order.original_size = yes_signal.target_size;
    pair.yes_order.remaining_size = yes_signal.target_size;
    pair.yes_order.created_at = now();

    // NO leg
    pair.no_order.client_order_id = generate_order_id();
    pair.no_order.strategy_name = no_signal.strategy_name;
    pair.no_order.market_id = no_signal.market_id;
    pair.no_order.token_id = no_signal.token_id;
    pair.no_order.side = no_signal.side;
    pair.no_order.type = OrderType::IOC;
    pair.no_order.price = no_signal.target_price;
    pair.no_order.original_size = no_signal.target_size;
    pair.no_order.remaining_size = no_signal.target_size;
    pair.no_order.created_at = now();

    // Store paired order
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        orders_[pair.yes_order.client_order_id] = pair.yes_order;
        orders_[pair.no_order.client_order_id] = pair.no_order;
        paired_orders_[pair.pair_id] = pair;
    }

    // Process based on mode
    switch (mode_) {
        case TradingMode::DRY_RUN:
            spdlog::info("[DRY-RUN] Would place paired order: YES {} @ {:.4f}, NO {} @ {:.4f}",
                        pair.yes_order.token_id, pair.yes_order.price,
                        pair.no_order.token_id, pair.no_order.price);
            break;

        case TradingMode::PAPER:
            process_paired_order(pair);
            break;

        case TradingMode::LIVE:
            // In live mode, we need to be very careful about paired execution
            spdlog::warn("[LIVE] Paired order requires atomic execution - using sequential IOC");
            send_live_order(pair.yes_order);
            send_live_order(pair.no_order);
            break;
    }

    risk_manager_->record_order_placed();
    risk_manager_->record_order_placed();
    orders_submitted_ += 2;

    result.accepted = true;
    result.order_id = pair.pair_id;

    return result;
}

bool ExecutionEngine::cancel_order(const std::string& order_id) {
    std::lock_guard<std::mutex> lock(orders_mutex_);

    auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        spdlog::warn("Order not found for cancellation: {}", order_id);
        return false;
    }

    Order& order = it->second;
    if (order.is_terminal()) {
        spdlog::warn("Cannot cancel terminal order: {}", order_id);
        return false;
    }

    if (mode_ == TradingMode::LIVE && polymarket_client_) {
        if (!polymarket_client_->cancel_order(order.exchange_order_id)) {
            spdlog::error("Failed to cancel order on exchange: {}", order_id);
            return false;
        }
    }

    order.mark_canceled();
    spdlog::info("Order canceled: {}", order_id);

    if (on_order_update_) {
        on_order_update_(order);
    }

    return true;
}

bool ExecutionEngine::cancel_all() {
    std::lock_guard<std::mutex> lock(orders_mutex_);

    int canceled = 0;
    for (auto& [id, order] : orders_) {
        if (!order.is_terminal()) {
            order.mark_canceled();
            canceled++;
            if (on_order_update_) {
                on_order_update_(order);
            }
        }
    }

    spdlog::info("Canceled {} orders", canceled);
    return true;
}

std::optional<Order> ExecutionEngine::get_order(const std::string& order_id) const {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    auto it = orders_.find(order_id);
    if (it != orders_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<Order> ExecutionEngine::get_open_orders() const {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    std::vector<Order> open;
    for (const auto& [id, order] : orders_) {
        if (!order.is_terminal()) {
            open.push_back(order);
        }
    }
    return open;
}

std::vector<Order> ExecutionEngine::get_orders_for_market(const std::string& market_id) const {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    std::vector<Order> result;
    for (const auto& [id, order] : orders_) {
        if (order.market_id == market_id) {
            result.push_back(order);
        }
    }
    return result;
}

LatencyMetrics ExecutionEngine::get_latency_metrics() const {
    std::lock_guard<std::mutex> lock(latency_mutex_);

    LatencyMetrics metrics;
    metrics.samples = decision_to_send_times_.size();

    if (metrics.samples == 0) {
        return metrics;
    }

    // Calculate percentiles
    auto sorted = decision_to_send_times_;
    std::sort(sorted.begin(), sorted.end());

    size_t p50_idx = sorted.size() * 50 / 100;
    size_t p95_idx = sorted.size() * 95 / 100;

    metrics.p50_decision_to_send = sorted[p50_idx];
    metrics.p95_decision_to_send = sorted[std::min(p95_idx, sorted.size() - 1)];

    return metrics;
}

void ExecutionEngine::send_live_order(Order& order) {
    if (!polymarket_client_) {
        spdlog::error("No Polymarket client available for live order");
        order.mark_rejected("No exchange connection");
        return;
    }

    if (!polymarket_client_->has_credentials()) {
        spdlog::error("No API credentials for live trading");
        order.mark_rejected("Missing API credentials");
        return;
    }

    auto start_time = now();

    PolymarketClient::OrderRequest req;
    req.token_id = order.token_id;
    req.side = order.side;
    req.price = order.price;
    req.size = order.original_size;
    req.type = order.type;

    order.mark_sent();

    auto response = polymarket_client_->place_order(req);

    auto send_duration = now() - start_time;
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        decision_to_send_times_.push_back(send_duration);
    }

    handle_order_response(order.client_order_id, response);
}

void ExecutionEngine::handle_order_response(const std::string& order_id,
                                            const PolymarketClient::OrderResponse& response) {
    std::lock_guard<std::mutex> lock(orders_mutex_);

    auto it = orders_.find(order_id);
    if (it == orders_.end()) return;

    Order& order = it->second;

    if (response.success) {
        order.mark_acknowledged(response.order_id, response.exchange_time_ms);
        spdlog::info("Order acknowledged: {} -> {}", order_id, response.order_id);
    } else {
        order.mark_rejected(response.error_message);
        spdlog::error("Order rejected: {} - {}", order_id, response.error_message);
    }

    if (on_order_update_) {
        on_order_update_(order);
    }
}

void ExecutionEngine::update_order_state(const std::string& order_id, OrderState new_state) {
    std::lock_guard<std::mutex> lock(orders_mutex_);

    auto it = orders_.find(order_id);
    if (it != orders_.end()) {
        it->second.state = new_state;
        if (on_order_update_) {
            on_order_update_(it->second);
        }
    }
}

void ExecutionEngine::record_fill(const std::string& order_id, const Fill& fill) {
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);

        auto it = orders_.find(order_id);
        if (it != orders_.end()) {
            it->second.mark_partial_fill(fill);
            if (it->second.state == OrderState::FILLED) {
                orders_filled_++;
            }
        }
    }

    risk_manager_->record_fill(fill);

    if (on_fill_) {
        on_fill_(fill);
    }
}

void ExecutionEngine::simulate_fill(Order& order) {
    // Simple simulation: fill at limit price with small random delay

    std::random_device rd;
    std::mt19937 gen(rd());

    // Simulate fill probability based on price aggressiveness
    // For now, assume 90% fill rate for limit orders
    std::uniform_real_distribution<> fill_prob(0.0, 1.0);
    if (fill_prob(gen) > 0.90) {
        order.mark_canceled();
        spdlog::info("[PAPER] Order expired unfilled: {}", order.client_order_id);
        return;
    }

    // Simulate partial fills
    std::uniform_real_distribution<> partial_prob(0.7, 1.0);
    double fill_ratio = partial_prob(gen);

    Fill fill;
    fill.order_id = order.client_order_id;
    fill.trade_id = generate_order_id();
    fill.market_id = order.market_id;
    fill.token_id = order.token_id;
    fill.side = order.side;
    fill.price = order.price;
    fill.size = order.original_size * fill_ratio;
    fill.notional = fill.price * fill.size;
    fill.fee = fill.notional * 0.02;  // 2% fee simulation
    fill.fill_time = now();
    fill.exchange_time_ms = now_ms();

    order.mark_partial_fill(fill);

    spdlog::info("[PAPER] Simulated fill: {} {} {:.2f} @ {:.4f}",
                order.client_order_id, side_to_string(order.side),
                fill.size, fill.price);

    if (on_fill_) {
        on_fill_(fill);
    }

    risk_manager_->record_fill(fill);
}

void ExecutionEngine::process_paired_order(PairedOrder& pair) {
    // For paper trading, simulate both sides
    simulate_fill(pair.yes_order);
    simulate_fill(pair.no_order);

    // Check if we need to unwind
    check_unwind_needed(pair);

    // Update stored orders
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        orders_[pair.yes_order.client_order_id] = pair.yes_order;
        orders_[pair.no_order.client_order_id] = pair.no_order;
    }
}

void ExecutionEngine::check_unwind_needed(PairedOrder& pair) {
    if (pair.needs_unwind()) {
        pair.state = PairedOrder::PairState::UNWINDING;

        // Determine which side needs unwinding
        if (pair.yes_order.state == OrderState::FILLED &&
            pair.no_order.state != OrderState::FILLED) {
            spdlog::warn("[PAPER] YES filled but NO did not - would need to sell YES to unwind");
            // In paper mode, just mark as completed with partial exposure
        } else if (pair.no_order.state == OrderState::FILLED &&
                   pair.yes_order.state != OrderState::FILLED) {
            spdlog::warn("[PAPER] NO filled but YES did not - would need to sell NO to unwind");
        }

        pair.state = PairedOrder::PairState::PARTIAL;
    } else if (pair.yes_order.state == OrderState::FILLED &&
               pair.no_order.state == OrderState::FILLED) {
        pair.state = PairedOrder::PairState::BOTH_FILLED;
        spdlog::info("[PAPER] Both sides of paired order filled successfully");
    }
}

void ExecutionEngine::paper_simulation_loop() {
    while (running_.load()) {
        std::string order_id;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !pending_paper_orders_.empty() || !running_.load();
            });

            if (!running_.load()) break;

            if (pending_paper_orders_.empty()) continue;

            order_id = pending_paper_orders_.front();
            pending_paper_orders_.pop();
        }

        // Simulate processing delay
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Process the order
        {
            std::lock_guard<std::mutex> lock(orders_mutex_);
            auto it = orders_.find(order_id);
            if (it != orders_.end() && !it->second.is_terminal()) {
                it->second.mark_acknowledged(generate_order_id(), now_ms());

                // Simulate fill after short delay
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                simulate_fill(it->second);

                if (on_order_update_) {
                    on_order_update_(it->second);
                }
            }
        }
    }
}

} // namespace arb
