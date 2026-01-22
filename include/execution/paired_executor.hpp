#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <functional>
#include <optional>
#include <memory>
#include "common/types.hpp"
#include "execution/order.hpp"

namespace arb {

// Forward declarations
class PolymarketClient;
class RiskManager;
class KillSwitch;

/**
 * State of a paired order execution.
 */
enum class PairState {
    CREATED,          // Just created, not submitted
    LEG1_PENDING,     // First leg submitted
    LEG1_FILLED,      // First leg filled, second pending
    LEG2_PENDING,     // Both legs submitted, waiting for second
    FULLY_FILLED,     // Both legs filled successfully
    PARTIAL_FILL,     // One leg partially filled
    LEG1_FAILED,      // First leg failed (safe - no exposure)
    LEG2_FAILED,      // Second leg failed (dangerous - unhedged)
    UNWIND_PENDING,   // Attempting to unwind first leg
    UNWOUND,          // Successfully unwound
    ABANDONED,        // Gave up (manual intervention needed)
    CANCELED          // Canceled before any fills
};

inline std::string pair_state_to_string(PairState s) {
    switch (s) {
        case PairState::CREATED: return "CREATED";
        case PairState::LEG1_PENDING: return "LEG1_PENDING";
        case PairState::LEG1_FILLED: return "LEG1_FILLED";
        case PairState::LEG2_PENDING: return "LEG2_PENDING";
        case PairState::FULLY_FILLED: return "FULLY_FILLED";
        case PairState::PARTIAL_FILL: return "PARTIAL_FILL";
        case PairState::LEG1_FAILED: return "LEG1_FAILED";
        case PairState::LEG2_FAILED: return "LEG2_FAILED";
        case PairState::UNWIND_PENDING: return "UNWIND_PENDING";
        case PairState::UNWOUND: return "UNWOUND";
        case PairState::ABANDONED: return "ABANDONED";
        case PairState::CANCELED: return "CANCELED";
    }
    return "UNKNOWN";
}

/**
 * A paired order (YES + NO legs for arbitrage).
 */
struct PairedOrderV2 {
    std::string pair_id;
    std::string market_id;

    // Leg 1 (typically YES)
    struct Leg {
        std::string order_id;
        std::string token_id;
        std::string outcome;  // "YES" or "NO"
        Side side;
        double price{0.0};
        double size{0.0};
        double filled_size{0.0};
        double avg_fill_price{0.0};
        OrderState state{OrderState::PENDING};
        int retry_count{0};
        WallClock submit_time;
        WallClock fill_time;
    };

    Leg leg1;
    Leg leg2;

    PairState state{PairState::CREATED};
    double expected_edge{0.0};
    double realized_edge{0.0};
    double realized_pnl{0.0};

    WallClock created_at;
    WallClock last_update;
    std::string failure_reason;

    // Calculated
    bool is_terminal() const;
    bool is_hedged() const;
    bool needs_unwind() const;
    double unhedged_exposure() const;
};

/**
 * Result of a paired execution attempt.
 */
struct PairedExecutionResult {
    bool success{false};
    std::string pair_id;
    PairState final_state{PairState::CREATED};
    double realized_pnl{0.0};
    std::string error;

    // Detailed breakdown
    bool leg1_filled{false};
    bool leg2_filled{false};
    double leg1_fill_price{0.0};
    double leg2_fill_price{0.0};
    double leg1_fill_size{0.0};
    double leg2_fill_size{0.0};
};

/**
 * Paired executor manages atomic YES+NO order pairs.
 *
 * DESIGN:
 * - Attempts to fill both legs atomically
 * - If leg1 fills but leg2 fails, attempts to unwind
 * - Tracks unhedged exposure and triggers kill switch if needed
 * - Supports retry with price adjustment
 * - Time-bounds all operations
 */
class PairedExecutor {
public:
    using UnwindCallback = std::function<void(const PairedOrderV2&, bool success)>;
    using FillCallback = std::function<void(const PairedOrderV2&, const Fill&)>;

    struct Config {
        std::chrono::milliseconds leg1_timeout{5000};
        std::chrono::milliseconds leg2_timeout{5000};
        std::chrono::milliseconds unwind_timeout{10000};
        int max_retries{3};
        double retry_price_adjustment_bps{10.0};  // Worsen price by 10bps on retry
        double max_price_adjustment_bps{50.0};    // Don't adjust more than 50bps
        double min_edge_after_adjustment{0.5};    // Cents - don't trade if edge too small
        bool auto_unwind{true};
        double unwind_price_discount_bps{25.0};   // Accept worse price to unwind
    };

    PairedExecutor(
        std::shared_ptr<PolymarketClient> client,
        std::shared_ptr<KillSwitch> kill_switch,
        const Config& config = Config{}
    );

    // Execute a paired order (blocking, with timeout)
    PairedExecutionResult execute(
        const Signal& yes_signal,
        const Signal& no_signal
    );

    // Execute async (non-blocking)
    std::string execute_async(
        const Signal& yes_signal,
        const Signal& no_signal
    );

    // Check status of async execution
    std::optional<PairedOrderV2> get_pair(const std::string& pair_id) const;

    // Manual intervention
    bool cancel_pair(const std::string& pair_id);
    bool force_unwind(const std::string& pair_id);

    // Callbacks
    void set_fill_callback(FillCallback cb) { on_fill_ = std::move(cb); }
    void set_unwind_callback(UnwindCallback cb) { on_unwind_ = std::move(cb); }

    // Stats
    int total_pairs() const { return total_pairs_.load(); }
    int successful_pairs() const { return successful_pairs_.load(); }
    int failed_pairs() const { return failed_pairs_.load(); }
    int unwind_attempts() const { return unwind_attempts_.load(); }
    double total_unhedged_exposure() const;

    // Get all active (non-terminal) pairs
    std::vector<PairedOrderV2> active_pairs() const;

    // Get all pairs needing unwind
    std::vector<PairedOrderV2> pairs_needing_unwind() const;

private:
    std::shared_ptr<PolymarketClient> client_;
    std::shared_ptr<KillSwitch> kill_switch_;
    Config config_;

    mutable std::mutex pairs_mutex_;
    std::map<std::string, PairedOrderV2> pairs_;

    FillCallback on_fill_;
    UnwindCallback on_unwind_;

    std::atomic<int> total_pairs_{0};
    std::atomic<int> successful_pairs_{0};
    std::atomic<int> failed_pairs_{0};
    std::atomic<int> unwind_attempts_{0};

    // Internal execution
    std::string generate_pair_id();
    bool submit_leg(PairedOrderV2& pair, PairedOrderV2::Leg& leg, bool is_leg1);
    bool wait_for_fill(PairedOrderV2& pair, PairedOrderV2::Leg& leg, std::chrono::milliseconds timeout);
    bool retry_leg(PairedOrderV2& pair, PairedOrderV2::Leg& leg, double current_edge);
    bool attempt_unwind(PairedOrderV2& pair);
    double calculate_adjusted_price(double original_price, Side side, int retry_count);
    double calculate_unwind_price(double entry_price, Side entry_side);
    void update_pair_state(PairedOrderV2& pair, PairState new_state);
    void check_kill_switch_trigger(const PairedOrderV2& pair);
};

} // namespace arb
