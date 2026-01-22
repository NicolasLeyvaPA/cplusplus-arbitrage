#include "core/reconciler.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

namespace arb {

bool ReconciliationResult::has_critical_discrepancies() const {
    for (const auto& d : discrepancies) {
        if (d.is_critical) return true;
    }
    return false;
}

std::string ReconciliationResult::summary() const {
    std::string s;
    s += fmt::format("Reconciliation {}: ", success ? "SUCCESS" : "FAILED");
    s += fmt::format("{} discrepancies, ", discrepancies.size());
    s += fmt::format("{} orders synced, ", orders_synced);
    s += fmt::format("{} positions synced, ", positions_synced);
    s += fmt::format("{} orders canceled", orders_canceled);
    if (!error_message.empty()) {
        s += fmt::format(" [Error: {}]", error_message);
    }
    return s;
}

Reconciler::Reconciler(
    std::shared_ptr<PolymarketClient> exchange_client,
    std::shared_ptr<StateManager> state_manager,
    const Config& config
)
    : exchange_client_(std::move(exchange_client))
    , state_manager_(std::move(state_manager))
    , config_(config)
{
    spdlog::info("Reconciler initialized: strategy={}",
                 config.default_strategy == ResolutionStrategy::TRUST_EXCHANGE ? "TRUST_EXCHANGE" :
                 config.default_strategy == ResolutionStrategy::TRUST_LOCAL ? "TRUST_LOCAL" :
                 config.default_strategy == ResolutionStrategy::MANUAL ? "MANUAL" : "CANCEL_ORPHANS");
}

ReconciliationResult Reconciler::reconcile() {
    spdlog::info("Starting reconciliation...");

    // Load local state
    auto local_state_opt = state_manager_->load_best_available();
    SystemState local_state;

    if (local_state_opt) {
        local_state = *local_state_opt;
        spdlog::info("Loaded local state: {} orders, {} positions, balance=${:.2f}",
                     local_state.open_orders.size(),
                     local_state.positions.size(),
                     local_state.balance);
    } else {
        spdlog::warn("No local state found, starting fresh");
    }

    return reconcile_with_state(local_state);
}

ReconciliationResult Reconciler::reconcile_with_state(const SystemState& local_state) {
    ReconciliationResult result;

    // Fetch exchange state
    auto exchange_state = fetch_exchange_state();
    if (!exchange_state.valid) {
        result.success = false;
        result.error_message = exchange_state.error;
        spdlog::error("Failed to fetch exchange state: {}", exchange_state.error);
        return result;
    }

    spdlog::info("Fetched exchange state: {} orders, {} positions, balance=${:.2f}",
                 exchange_state.open_orders.size(),
                 exchange_state.positions.size(),
                 exchange_state.balance);

    // Compare orders
    auto order_discrepancies = compare_orders(local_state.open_orders, exchange_state.open_orders);
    for (auto& d : order_discrepancies) {
        result.discrepancies.push_back(std::move(d));
    }

    // Compare positions
    auto position_discrepancies = compare_positions(local_state.positions, exchange_state.positions);
    for (auto& d : position_discrepancies) {
        result.discrepancies.push_back(std::move(d));
    }

    // Compare balance
    auto balance_discrepancy = compare_balance(local_state.balance, exchange_state.balance);
    if (balance_discrepancy) {
        result.discrepancies.push_back(*balance_discrepancy);
    }

    // Log discrepancies
    if (!result.discrepancies.empty()) {
        spdlog::warn("Found {} discrepancies during reconciliation:", result.discrepancies.size());
        for (const auto& d : result.discrepancies) {
            spdlog::warn("  - {}: {} local='{}' remote='{}' {}",
                        discrepancy_to_string(d.type),
                        d.identifier,
                        d.local_value,
                        d.remote_value,
                        d.is_critical ? "[CRITICAL]" : "");
        }
    }

    // Check for critical discrepancies
    if (result.has_critical_discrepancies() && config_.require_approval_for_critical) {
        if (approval_callback_) {
            bool approved = approval_callback_(result.discrepancies);
            if (!approved) {
                result.success = false;
                result.error_message = "Operator did not approve critical discrepancies";
                return result;
            }
        } else {
            spdlog::warn("Critical discrepancies found but no approval callback set");
        }
    }

    // Resolve based on strategy
    switch (config_.default_strategy) {
        case ResolutionStrategy::TRUST_EXCHANGE:
        case ResolutionStrategy::CANCEL_ORPHANS: {
            result.resolved_state = resolve_to_exchange(exchange_state, local_state);
            result.orders_synced = static_cast<int>(exchange_state.open_orders.size());
            result.positions_synced = static_cast<int>(exchange_state.positions.size());

            // Cancel orphan orders if configured
            if (config_.cancel_orphan_orders) {
                for (const auto& d : result.discrepancies) {
                    if (d.type == DiscrepancyType::MISSING_LOCAL_ORDER) {
                        if (cancel_orphan_order(d.identifier)) {
                            result.orders_canceled++;
                        }
                    }
                }
            }
            break;
        }

        case ResolutionStrategy::TRUST_LOCAL: {
            // Just use local state (risky, for testing only)
            result.resolved_state = local_state;
            spdlog::warn("Using TRUST_LOCAL strategy - exchange state ignored!");
            break;
        }

        case ResolutionStrategy::MANUAL: {
            // Don't resolve automatically
            if (!result.discrepancies.empty()) {
                result.success = false;
                result.error_message = "Manual resolution required for discrepancies";
                return result;
            }
            result.resolved_state = local_state;
            break;
        }
    }

    // Save resolved state
    if (result.resolved_state) {
        // Update state manager with resolved state
        state_manager_->update_balance(result.resolved_state->balance);
        state_manager_->update_daily_pnl(result.resolved_state->daily_pnl);
        state_manager_->save();
    }

    result.success = true;
    result.is_consistent = result.discrepancies.empty();

    spdlog::info("Reconciliation complete: {}", result.summary());
    return result;
}

Reconciler::ExchangeState Reconciler::fetch_exchange_state() {
    ExchangeState state;

    try {
        // Fetch open orders
        auto orders_response = exchange_client_->get_open_orders();
        if (!orders_response.success) {
            state.error = "Failed to fetch open orders: " + orders_response.error;
            return state;
        }

        for (const auto& order : orders_response.orders) {
            PersistedOrder po;
            po.order_id = order.order_id;
            po.client_order_id = order.client_order_id;
            po.market_id = order.market_id;
            po.token_id = order.token_id;
            po.side = order.side;
            po.order_type = order.order_type;
            po.state = order.state;
            po.price = order.price;
            po.size = order.original_size;
            po.filled_size = order.filled_size;
            po.created_at = order.created_at;
            po.last_update = wall_now();
            state.open_orders.push_back(po);
        }

        // Fetch positions
        auto positions_response = exchange_client_->get_positions();
        if (!positions_response.success) {
            state.error = "Failed to fetch positions: " + positions_response.error;
            return state;
        }

        for (const auto& pos : positions_response.positions) {
            PersistedPosition pp;
            pp.market_id = pos.market_id;
            pp.token_id = pos.token_id;
            pp.outcome = pos.outcome;
            pp.size = pos.size;
            pp.entry_price = pos.avg_price;
            pp.cost_basis = pos.size * pos.avg_price;
            pp.unrealized_pnl = pos.unrealized_pnl;
            pp.realized_pnl = pos.realized_pnl;
            pp.last_update = wall_now();
            state.positions.push_back(pp);
        }

        // Fetch balance
        auto balance_response = exchange_client_->get_balance();
        if (!balance_response.success) {
            state.error = "Failed to fetch balance: " + balance_response.error;
            return state;
        }
        state.balance = balance_response.available;

        state.valid = true;

    } catch (const std::exception& e) {
        state.error = std::string("Exception: ") + e.what();
    }

    return state;
}

std::vector<Discrepancy> Reconciler::compare_orders(
    const std::vector<PersistedOrder>& local,
    const std::vector<PersistedOrder>& remote
) {
    std::vector<Discrepancy> discrepancies;

    // Build lookup maps
    std::map<std::string, const PersistedOrder*> local_map;
    for (const auto& o : local) {
        local_map[o.order_id] = &o;
    }

    std::map<std::string, const PersistedOrder*> remote_map;
    for (const auto& o : remote) {
        remote_map[o.order_id] = &o;
    }

    // Find orders in remote but not local
    for (const auto& [id, order] : remote_map) {
        if (local_map.find(id) == local_map.end()) {
            Discrepancy d;
            d.type = DiscrepancyType::MISSING_LOCAL_ORDER;
            d.identifier = id;
            d.local_value = "not present";
            d.remote_value = fmt::format("{}@{:.4f} x {:.2f}",
                                        side_to_string(order->side),
                                        order->price, order->size);
            d.details = "Order exists on exchange but not in local state";
            d.is_critical = true;  // Could indicate missed fills
            discrepancies.push_back(d);
        }
    }

    // Find orders in local but not remote
    for (const auto& [id, order] : local_map) {
        if (remote_map.find(id) == remote_map.end()) {
            // Only report if order was in active state
            if (order->state == OrderState::SENT ||
                order->state == OrderState::ACKNOWLEDGED ||
                order->state == OrderState::PARTIAL) {

                Discrepancy d;
                d.type = DiscrepancyType::MISSING_REMOTE_ORDER;
                d.identifier = id;
                d.local_value = fmt::format("{}@{:.4f} x {:.2f} ({})",
                                           side_to_string(order->side),
                                           order->price, order->size,
                                           order_state_to_string(order->state));
                d.remote_value = "not present";
                d.details = "Order in local state not found on exchange - may have filled or been canceled";
                d.is_critical = true;
                discrepancies.push_back(d);
            }
        }
    }

    // Compare matching orders
    for (const auto& [id, local_order] : local_map) {
        auto it = remote_map.find(id);
        if (it != remote_map.end()) {
            const auto* remote_order = it->second;
            if (!orders_match(*local_order, *remote_order)) {
                Discrepancy d;
                d.type = DiscrepancyType::ORDER_STATE_MISMATCH;
                d.identifier = id;
                d.local_value = fmt::format("{} filled={:.2f}",
                                           order_state_to_string(local_order->state),
                                           local_order->filled_size);
                d.remote_value = fmt::format("{} filled={:.2f}",
                                            order_state_to_string(remote_order->state),
                                            remote_order->filled_size);
                d.details = "Order state differs between local and exchange";
                d.is_critical = (local_order->filled_size != remote_order->filled_size);
                discrepancies.push_back(d);
            }
        }
    }

    return discrepancies;
}

std::vector<Discrepancy> Reconciler::compare_positions(
    const std::vector<PersistedPosition>& local,
    const std::vector<PersistedPosition>& remote
) {
    std::vector<Discrepancy> discrepancies;

    // Build lookup maps
    std::map<std::string, const PersistedPosition*> local_map;
    for (const auto& p : local) {
        local_map[p.token_id] = &p;
    }

    std::map<std::string, const PersistedPosition*> remote_map;
    for (const auto& p : remote) {
        remote_map[p.token_id] = &p;
    }

    // Find positions in remote but not local
    for (const auto& [token_id, pos] : remote_map) {
        if (local_map.find(token_id) == local_map.end() && pos->size > 0.001) {
            Discrepancy d;
            d.type = DiscrepancyType::UNKNOWN_POSITION;
            d.identifier = token_id;
            d.local_value = "0";
            d.remote_value = fmt::format("{:.4f}", pos->size);
            d.details = "Position exists on exchange but not tracked locally";
            d.is_critical = true;
            discrepancies.push_back(d);
        }
    }

    // Compare positions
    for (const auto& [token_id, local_pos] : local_map) {
        auto it = remote_map.find(token_id);
        double remote_size = (it != remote_map.end()) ? it->second->size : 0.0;

        if (!positions_match(*local_pos, it != remote_map.end() ? *it->second : PersistedPosition{})) {
            Discrepancy d;
            d.type = DiscrepancyType::POSITION_SIZE_MISMATCH;
            d.identifier = token_id;
            d.local_value = fmt::format("{:.4f}", local_pos->size);
            d.remote_value = fmt::format("{:.4f}", remote_size);
            d.details = "Position size differs between local and exchange";
            d.is_critical = std::abs(local_pos->size - remote_size) > 0.01;
            discrepancies.push_back(d);
        }
    }

    return discrepancies;
}

std::optional<Discrepancy> Reconciler::compare_balance(double local, double remote) {
    double diff = std::abs(local - remote);
    double tolerance = remote * config_.balance_tolerance_percent;

    if (diff > tolerance && diff > 0.01) {  // At least 1 cent difference
        Discrepancy d;
        d.type = DiscrepancyType::BALANCE_MISMATCH;
        d.identifier = "balance";
        d.local_value = fmt::format("{:.2f}", local);
        d.remote_value = fmt::format("{:.2f}", remote);
        d.details = fmt::format("Balance differs by ${:.2f} ({:.1f}%)",
                               diff, (diff / remote) * 100);
        d.is_critical = (diff / remote) > 0.05;  // >5% is critical
        return d;
    }

    return std::nullopt;
}

SystemState Reconciler::resolve_to_exchange(
    const ExchangeState& exchange_state,
    const SystemState& local_state
) {
    SystemState resolved = local_state;

    // Use exchange orders
    resolved.open_orders = exchange_state.open_orders;

    // Use exchange positions
    resolved.positions = exchange_state.positions;

    // Use exchange balance
    resolved.balance = exchange_state.balance;

    // Recalculate exposure
    double exposure = 0.0;
    for (const auto& pos : resolved.positions) {
        exposure += pos.size * pos.entry_price;
    }
    resolved.total_exposure = exposure;

    resolved.last_save = wall_now();

    return resolved;
}

bool Reconciler::cancel_orphan_order(const std::string& order_id) {
    spdlog::warn("Canceling orphan order: {}", order_id);

    auto response = exchange_client_->cancel_order(order_id);
    if (response.success) {
        spdlog::info("Successfully canceled orphan order: {}", order_id);
        return true;
    } else {
        spdlog::error("Failed to cancel orphan order {}: {}", order_id, response.error);
        return false;
    }
}

bool Reconciler::orders_match(const PersistedOrder& a, const PersistedOrder& b) {
    // Orders match if key fields are the same
    return a.order_id == b.order_id &&
           a.state == b.state &&
           std::abs(a.filled_size - b.filled_size) < 0.0001;
}

bool Reconciler::positions_match(const PersistedPosition& a, const PersistedPosition& b) {
    // Positions match if size is within tolerance
    return std::abs(a.size - b.size) < 0.0001;
}

bool Reconciler::resolve_discrepancy(const Discrepancy& discrepancy, ResolutionStrategy strategy) {
    spdlog::info("Resolving discrepancy: {} {} with strategy {}",
                discrepancy_to_string(discrepancy.type),
                discrepancy.identifier,
                strategy == ResolutionStrategy::TRUST_EXCHANGE ? "TRUST_EXCHANGE" : "other");

    // For now, just log - actual resolution happens in reconcile()
    return true;
}

// ReconciliationGuard implementation
ReconciliationGuard::ReconciliationGuard(Reconciler& reconciler) {
    result_ = reconciler.reconcile();
    ready_ = result_.success && !result_.has_critical_discrepancies();

    if (!ready_) {
        spdlog::error("ReconciliationGuard: System not ready for trading");
        spdlog::error("  Result: {}", result_.summary());
    }
}

} // namespace arb
