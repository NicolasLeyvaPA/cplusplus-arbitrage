#pragma once

#include <string>
#include <optional>
#include <chrono>
#include <vector>
#include "common/types.hpp"

namespace arb {

/**
 * Order representation with full lifecycle tracking.
 */
struct Order {
    // Identifiers
    std::string client_order_id;   // Our internal ID
    std::string exchange_order_id; // Exchange-assigned ID (after ACK)
    std::string strategy_name;

    // Order details
    std::string market_id;
    std::string token_id;
    Side side;
    OrderType type;
    Price price;
    Size original_size;
    Size filled_size{0.0};
    Size remaining_size;

    // State
    OrderState state{OrderState::PENDING};

    // Timing
    Timestamp created_at;
    Timestamp sent_at;
    Timestamp acked_at;
    Timestamp last_fill_at;
    Timestamp completed_at;
    int64_t exchange_ack_time_ms{0};

    // Fills
    std::vector<Fill> fills;

    // Fees
    Notional total_fees{0.0};

    // Error tracking
    std::string reject_reason;
    int retry_count{0};

    // Computed values
    Price average_fill_price() const;
    Notional filled_notional() const;
    bool is_terminal() const;
    Duration time_to_ack() const;
    Duration time_to_fill() const;

    // State transitions
    void mark_sent();
    void mark_acknowledged(const std::string& exchange_id, int64_t exchange_time_ms);
    void mark_partial_fill(const Fill& fill);
    void mark_filled();
    void mark_canceled();
    void mark_rejected(const std::string& reason);
};

/**
 * Generate unique client order ID.
 */
std::string generate_order_id();

/**
 * Paired order for two-outcome strategy.
 * Tracks both YES and NO legs.
 */
struct PairedOrder {
    std::string pair_id;
    Order yes_order;
    Order no_order;

    enum class PairState {
        PENDING,
        YES_SENT,
        NO_SENT,
        BOTH_SENT,
        YES_FILLED,
        NO_FILLED,
        BOTH_FILLED,
        PARTIAL,       // One filled, other not
        UNWINDING,     // Attempting to unwind partial
        COMPLETED,
        FAILED
    };

    PairState state{PairState::PENDING};
    Timestamp created_at;

    // Calculate net exposure if only one side fills
    double net_exposure() const;

    // Check if needs unwinding
    bool needs_unwind() const;
};

} // namespace arb
