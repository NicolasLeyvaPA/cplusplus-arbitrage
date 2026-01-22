#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include "common/types.hpp"
#include "core/state_manager.hpp"
#include "market_data/polymarket_client.hpp"

namespace arb {

/**
 * Discrepancy types found during reconciliation.
 */
enum class DiscrepancyType {
    MISSING_LOCAL_ORDER,     // Order on exchange not in local state
    MISSING_REMOTE_ORDER,    // Order in local state not on exchange
    ORDER_STATE_MISMATCH,    // Order exists but state differs
    POSITION_SIZE_MISMATCH,  // Position size differs
    BALANCE_MISMATCH,        // Balance differs
    UNKNOWN_POSITION         // Position on exchange not tracked locally
};

inline std::string discrepancy_to_string(DiscrepancyType d) {
    switch (d) {
        case DiscrepancyType::MISSING_LOCAL_ORDER: return "MISSING_LOCAL_ORDER";
        case DiscrepancyType::MISSING_REMOTE_ORDER: return "MISSING_REMOTE_ORDER";
        case DiscrepancyType::ORDER_STATE_MISMATCH: return "ORDER_STATE_MISMATCH";
        case DiscrepancyType::POSITION_SIZE_MISMATCH: return "POSITION_SIZE_MISMATCH";
        case DiscrepancyType::BALANCE_MISMATCH: return "BALANCE_MISMATCH";
        case DiscrepancyType::UNKNOWN_POSITION: return "UNKNOWN_POSITION";
    }
    return "UNKNOWN";
}

/**
 * A single discrepancy found during reconciliation.
 */
struct Discrepancy {
    DiscrepancyType type;
    std::string identifier;  // order_id, token_id, or "balance"
    std::string local_value;
    std::string remote_value;
    std::string details;
    bool is_critical{false};  // If true, trading should not proceed
};

/**
 * Result of reconciliation process.
 */
struct ReconciliationResult {
    bool success{false};
    bool is_consistent{false};
    std::vector<Discrepancy> discrepancies;

    // Resolved state (what we should use)
    std::optional<SystemState> resolved_state;

    // Summary
    int orders_synced{0};
    int positions_synced{0};
    int orders_canceled{0};  // Orphaned orders canceled

    std::string error_message;

    // Helpers
    bool has_critical_discrepancies() const;
    std::string summary() const;
};

/**
 * Resolution strategy for discrepancies.
 */
enum class ResolutionStrategy {
    TRUST_EXCHANGE,    // Always use exchange state (safest)
    TRUST_LOCAL,       // Use local state (risky, only for testing)
    MANUAL,            // Require operator intervention
    CANCEL_ORPHANS     // Cancel any orders not in local state
};

/**
 * Reconciler synchronizes local state with exchange state on startup.
 *
 * DESIGN:
 * - Run at startup BEFORE trading begins
 * - Queries exchange for current orders, positions, balance
 * - Compares with saved state
 * - Resolves discrepancies based on strategy
 * - Updates local state to match resolved state
 *
 * SAFETY:
 * - If critical discrepancies found, blocks trading until resolved
 * - Always logs all discrepancies for audit
 * - Never automatically modifies exchange state in TRUST_EXCHANGE mode
 */
class Reconciler {
public:
    using ApprovalCallback = std::function<bool(const std::vector<Discrepancy>&)>;

    struct Config {
        ResolutionStrategy default_strategy{ResolutionStrategy::TRUST_EXCHANGE};
        bool cancel_orphan_orders{true};
        bool require_approval_for_critical{true};
        double balance_tolerance_percent{0.01};  // 1% tolerance
        std::chrono::seconds timeout{30};
    };

    Reconciler(
        std::shared_ptr<PolymarketClient> exchange_client,
        std::shared_ptr<StateManager> state_manager,
        const Config& config = Config{}
    );

    // Main reconciliation entry point
    ReconciliationResult reconcile();

    // Reconcile with explicit local state (for testing)
    ReconciliationResult reconcile_with_state(const SystemState& local_state);

    // Set callback for approval of critical discrepancies
    void set_approval_callback(ApprovalCallback cb) { approval_callback_ = std::move(cb); }

    // Manual resolution
    bool resolve_discrepancy(const Discrepancy& discrepancy, ResolutionStrategy strategy);

    // Query exchange state (public for testing)
    struct ExchangeState {
        std::vector<PersistedOrder> open_orders;
        std::vector<PersistedPosition> positions;
        double balance{0.0};
        bool valid{false};
        std::string error;
    };

    ExchangeState fetch_exchange_state();

private:
    std::shared_ptr<PolymarketClient> exchange_client_;
    std::shared_ptr<StateManager> state_manager_;
    Config config_;
    ApprovalCallback approval_callback_;

    // Comparison functions
    std::vector<Discrepancy> compare_orders(
        const std::vector<PersistedOrder>& local,
        const std::vector<PersistedOrder>& remote
    );

    std::vector<Discrepancy> compare_positions(
        const std::vector<PersistedPosition>& local,
        const std::vector<PersistedPosition>& remote
    );

    std::optional<Discrepancy> compare_balance(double local, double remote);

    // Resolution functions
    SystemState resolve_to_exchange(
        const ExchangeState& exchange_state,
        const SystemState& local_state
    );

    bool cancel_orphan_order(const std::string& order_id);

    // Helpers
    bool orders_match(const PersistedOrder& a, const PersistedOrder& b);
    bool positions_match(const PersistedPosition& a, const PersistedPosition& b);
};

/**
 * RAII guard that ensures reconciliation before trading.
 * Blocks construction if reconciliation fails.
 */
class ReconciliationGuard {
public:
    ReconciliationGuard(Reconciler& reconciler);

    bool is_ready() const { return ready_; }
    const ReconciliationResult& result() const { return result_; }

private:
    ReconciliationResult result_;
    bool ready_{false};
};

} // namespace arb
