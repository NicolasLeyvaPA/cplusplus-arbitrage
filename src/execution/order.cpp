#include "execution/order.hpp"
#include <random>
#include <sstream>
#include <iomanip>

namespace arb {

Price Order::average_fill_price() const {
    if (filled_size <= 0.0) return 0.0;

    double total_value = 0.0;
    for (const auto& fill : fills) {
        total_value += fill.price * fill.size;
    }
    return total_value / filled_size;
}

Notional Order::filled_notional() const {
    return filled_size * average_fill_price();
}

bool Order::is_terminal() const {
    return state == OrderState::FILLED ||
           state == OrderState::CANCELED ||
           state == OrderState::REJECTED ||
           state == OrderState::EXPIRED;
}

Duration Order::time_to_ack() const {
    if (state == OrderState::PENDING || state == OrderState::SENT) {
        return Duration::zero();
    }
    return acked_at - sent_at;
}

Duration Order::time_to_fill() const {
    if (fills.empty()) return Duration::zero();
    return last_fill_at - sent_at;
}

void Order::mark_sent() {
    state = OrderState::SENT;
    sent_at = now();
}

void Order::mark_acknowledged(const std::string& exchange_id, int64_t exchange_time_ms) {
    exchange_order_id = exchange_id;
    exchange_ack_time_ms = exchange_time_ms;
    state = OrderState::ACKNOWLEDGED;
    acked_at = now();
}

void Order::mark_partial_fill(const Fill& fill) {
    fills.push_back(fill);
    filled_size += fill.size;
    remaining_size = original_size - filled_size;
    total_fees += fill.fee;
    last_fill_at = now();

    if (remaining_size <= 0.0001) {
        state = OrderState::FILLED;
        completed_at = now();
    } else {
        state = OrderState::PARTIAL;
    }
}

void Order::mark_filled() {
    state = OrderState::FILLED;
    completed_at = now();
    remaining_size = 0.0;
}

void Order::mark_canceled() {
    state = OrderState::CANCELED;
    completed_at = now();
}

void Order::mark_rejected(const std::string& reason) {
    state = OrderState::REJECTED;
    reject_reason = reason;
    completed_at = now();
}

std::string generate_order_id() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    std::stringstream ss;
    ss << "ORD-" << std::hex << std::setw(16) << std::setfill('0') << dis(gen);
    return ss.str();
}

double PairedOrder::net_exposure() const {
    if (yes_order.state == OrderState::FILLED && no_order.state != OrderState::FILLED) {
        return yes_order.filled_notional();
    }
    if (no_order.state == OrderState::FILLED && yes_order.state != OrderState::FILLED) {
        return no_order.filled_notional();
    }
    return 0.0;
}

bool PairedOrder::needs_unwind() const {
    // Need unwind if one side filled and the other didn't
    bool yes_done = yes_order.is_terminal();
    bool no_done = no_order.is_terminal();

    if (yes_done && no_done) {
        // Check if one filled and other didn't
        return (yes_order.state == OrderState::FILLED && no_order.state != OrderState::FILLED) ||
               (no_order.state == OrderState::FILLED && yes_order.state != OrderState::FILLED);
    }

    return false;
}

} // namespace arb
