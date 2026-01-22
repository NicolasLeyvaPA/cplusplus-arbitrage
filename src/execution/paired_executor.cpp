#include "execution/paired_executor.hpp"
#include "core/kill_switch.hpp"
#include "market_data/polymarket_client.hpp"
#include <spdlog/spdlog.h>
#include <random>
#include <thread>

namespace arb {

bool PairedOrderV2::is_terminal() const {
    return state == PairState::FULLY_FILLED ||
           state == PairState::UNWOUND ||
           state == PairState::ABANDONED ||
           state == PairState::CANCELED ||
           state == PairState::LEG1_FAILED;
}

bool PairedOrderV2::is_hedged() const {
    return state == PairState::FULLY_FILLED ||
           state == PairState::CREATED ||
           state == PairState::CANCELED ||
           state == PairState::LEG1_FAILED ||
           state == PairState::UNWOUND;
}

bool PairedOrderV2::needs_unwind() const {
    return state == PairState::LEG2_FAILED ||
           state == PairState::PARTIAL_FILL;
}

double PairedOrderV2::unhedged_exposure() const {
    if (is_hedged()) return 0.0;

    // Calculate the difference between leg fills
    double leg1_exposure = leg1.filled_size * leg1.avg_fill_price;
    double leg2_exposure = leg2.filled_size * leg2.avg_fill_price;

    return std::abs(leg1_exposure - leg2_exposure);
}

PairedExecutor::PairedExecutor(
    std::shared_ptr<PolymarketClient> client,
    std::shared_ptr<KillSwitch> kill_switch,
    const Config& config
)
    : client_(std::move(client))
    , kill_switch_(std::move(kill_switch))
    , config_(config)
{
    spdlog::info("PairedExecutor initialized: leg1_timeout={}ms, leg2_timeout={}ms, max_retries={}",
                 config.leg1_timeout.count(),
                 config.leg2_timeout.count(),
                 config.max_retries);
}

PairedExecutionResult PairedExecutor::execute(
    const Signal& yes_signal,
    const Signal& no_signal
) {
    PairedExecutionResult result;

    // Create pair
    PairedOrderV2 pair;
    pair.pair_id = generate_pair_id();
    pair.market_id = yes_signal.market_id;
    pair.created_at = wall_now();
    pair.expected_edge = yes_signal.expected_edge + no_signal.expected_edge;

    // Initialize legs
    pair.leg1.token_id = yes_signal.token_id;
    pair.leg1.outcome = "YES";
    pair.leg1.side = yes_signal.side;
    pair.leg1.price = yes_signal.target_price;
    pair.leg1.size = yes_signal.target_size;

    pair.leg2.token_id = no_signal.token_id;
    pair.leg2.outcome = "NO";
    pair.leg2.side = no_signal.side;
    pair.leg2.price = no_signal.target_price;
    pair.leg2.size = no_signal.target_size;

    result.pair_id = pair.pair_id;
    total_pairs_++;

    spdlog::info("Executing paired order {}: YES@{:.4f} x {:.2f}, NO@{:.4f} x {:.2f}, expected_edge={:.2f}c",
                 pair.pair_id,
                 pair.leg1.price, pair.leg1.size,
                 pair.leg2.price, pair.leg2.size,
                 pair.expected_edge * 100);

    // Store pair
    {
        std::lock_guard<std::mutex> lock(pairs_mutex_);
        pairs_[pair.pair_id] = pair;
    }

    // Submit leg 1
    update_pair_state(pair, PairState::LEG1_PENDING);
    if (!submit_leg(pair, pair.leg1, true)) {
        // Leg 1 submission failed - safe, no exposure
        update_pair_state(pair, PairState::LEG1_FAILED);
        result.error = "Leg 1 submission failed";
        failed_pairs_++;
        return result;
    }

    // Wait for leg 1 fill
    if (!wait_for_fill(pair, pair.leg1, config_.leg1_timeout)) {
        // Leg 1 didn't fill in time
        // Try to cancel
        if (client_->cancel_order(pair.leg1.order_id).success) {
            update_pair_state(pair, PairState::CANCELED);
            result.error = "Leg 1 timeout, canceled";
        } else {
            // Couldn't cancel - might have filled
            // Re-check state
            auto order_state = client_->get_order(pair.leg1.order_id);
            if (order_state && order_state->filled_size > 0) {
                // It filled! Need to continue with leg 2
                pair.leg1.filled_size = order_state->filled_size;
                pair.leg1.state = OrderState::FILLED;
                update_pair_state(pair, PairState::LEG1_FILLED);
            } else {
                update_pair_state(pair, PairState::LEG1_FAILED);
                result.error = "Leg 1 timeout, cancel failed";
                failed_pairs_++;
                return result;
            }
        }
    } else {
        update_pair_state(pair, PairState::LEG1_FILLED);
    }

    result.leg1_filled = (pair.leg1.filled_size > 0);
    result.leg1_fill_price = pair.leg1.avg_fill_price;
    result.leg1_fill_size = pair.leg1.filled_size;

    if (pair.state == PairState::CANCELED) {
        return result;
    }

    // Submit leg 2
    update_pair_state(pair, PairState::LEG2_PENDING);
    if (!submit_leg(pair, pair.leg2, false)) {
        // Leg 2 submission failed - dangerous, we have exposure!
        spdlog::error("CRITICAL: Leg 2 submission failed, unhedged exposure!");
        update_pair_state(pair, PairState::LEG2_FAILED);

        // Attempt unwind
        if (config_.auto_unwind) {
            if (attempt_unwind(pair)) {
                result.error = "Leg 2 failed, unwound successfully";
            } else {
                result.error = "Leg 2 failed, unwind FAILED - MANUAL INTERVENTION NEEDED";
                check_kill_switch_trigger(pair);
            }
        } else {
            result.error = "Leg 2 failed, auto-unwind disabled";
            check_kill_switch_trigger(pair);
        }

        failed_pairs_++;
        result.final_state = pair.state;
        return result;
    }

    // Wait for leg 2 fill with retries
    int retries = 0;
    double current_edge = pair.expected_edge;

    while (!wait_for_fill(pair, pair.leg2, config_.leg2_timeout) && retries < config_.max_retries) {
        spdlog::warn("Leg 2 not filled, retry {}/{}", retries + 1, config_.max_retries);

        // Cancel current order
        client_->cancel_order(pair.leg2.order_id);

        // Retry with adjusted price
        if (retry_leg(pair, pair.leg2, current_edge)) {
            retries++;
        } else {
            break;  // Can't retry further
        }
    }

    if (pair.leg2.filled_size >= pair.leg2.size * 0.99) {  // 99% fill threshold
        update_pair_state(pair, PairState::FULLY_FILLED);
        result.success = true;
        successful_pairs_++;

        // Calculate realized PnL
        pair.realized_pnl = (pair.leg1.filled_size * pair.leg1.avg_fill_price +
                            pair.leg2.filled_size * pair.leg2.avg_fill_price) -
                           (pair.leg1.size * pair.leg1.price +
                            pair.leg2.size * pair.leg2.price);

        spdlog::info("Paired order {} FILLED: realized_pnl=${:.4f}",
                     pair.pair_id, pair.realized_pnl);
    } else {
        // Partial or no fill on leg 2
        spdlog::error("Leg 2 failed after {} retries, attempting unwind", retries);
        update_pair_state(pair, PairState::LEG2_FAILED);

        if (config_.auto_unwind && attempt_unwind(pair)) {
            result.error = "Leg 2 failed, unwound successfully";
        } else {
            result.error = "Leg 2 failed, unwind FAILED";
            check_kill_switch_trigger(pair);
        }

        failed_pairs_++;
    }

    result.leg2_filled = (pair.leg2.filled_size > 0);
    result.leg2_fill_price = pair.leg2.avg_fill_price;
    result.leg2_fill_size = pair.leg2.filled_size;
    result.final_state = pair.state;
    result.realized_pnl = pair.realized_pnl;

    // Update stored pair
    {
        std::lock_guard<std::mutex> lock(pairs_mutex_);
        pairs_[pair.pair_id] = pair;
    }

    return result;
}

std::string PairedExecutor::execute_async(
    const Signal& yes_signal,
    const Signal& no_signal
) {
    // Create pair ID first
    std::string pair_id = generate_pair_id();

    // Launch in background thread
    std::thread([this, yes_signal, no_signal, pair_id]() {
        // Modify signals with our pair_id for tracking
        auto result = execute(yes_signal, no_signal);
        // Result is stored in pairs_ map
    }).detach();

    return pair_id;
}

std::optional<PairedOrderV2> PairedExecutor::get_pair(const std::string& pair_id) const {
    std::lock_guard<std::mutex> lock(pairs_mutex_);
    auto it = pairs_.find(pair_id);
    if (it != pairs_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool PairedExecutor::cancel_pair(const std::string& pair_id) {
    std::lock_guard<std::mutex> lock(pairs_mutex_);

    auto it = pairs_.find(pair_id);
    if (it == pairs_.end()) return false;

    auto& pair = it->second;
    if (pair.is_terminal()) return false;

    // Try to cancel both legs
    bool canceled = false;
    if (!pair.leg1.order_id.empty() && pair.leg1.state != OrderState::FILLED) {
        if (client_->cancel_order(pair.leg1.order_id).success) {
            canceled = true;
        }
    }
    if (!pair.leg2.order_id.empty() && pair.leg2.state != OrderState::FILLED) {
        if (client_->cancel_order(pair.leg2.order_id).success) {
            canceled = true;
        }
    }

    if (canceled) {
        pair.state = PairState::CANCELED;
    }

    return canceled;
}

bool PairedExecutor::force_unwind(const std::string& pair_id) {
    std::lock_guard<std::mutex> lock(pairs_mutex_);

    auto it = pairs_.find(pair_id);
    if (it == pairs_.end()) return false;

    return attempt_unwind(it->second);
}

double PairedExecutor::total_unhedged_exposure() const {
    std::lock_guard<std::mutex> lock(pairs_mutex_);

    double total = 0.0;
    for (const auto& [id, pair] : pairs_) {
        total += pair.unhedged_exposure();
    }
    return total;
}

std::vector<PairedOrderV2> PairedExecutor::active_pairs() const {
    std::lock_guard<std::mutex> lock(pairs_mutex_);

    std::vector<PairedOrderV2> active;
    for (const auto& [id, pair] : pairs_) {
        if (!pair.is_terminal()) {
            active.push_back(pair);
        }
    }
    return active;
}

std::vector<PairedOrderV2> PairedExecutor::pairs_needing_unwind() const {
    std::lock_guard<std::mutex> lock(pairs_mutex_);

    std::vector<PairedOrderV2> needing_unwind;
    for (const auto& [id, pair] : pairs_) {
        if (pair.needs_unwind()) {
            needing_unwind.push_back(pair);
        }
    }
    return needing_unwind;
}

std::string PairedExecutor::generate_pair_id() {
    static std::atomic<int> counter{0};
    auto now_ms = now_ms();
    return fmt::format("PAIR-{}-{}", now_ms, counter++);
}

bool PairedExecutor::submit_leg(PairedOrderV2& pair, PairedOrderV2::Leg& leg, bool is_leg1) {
    try {
        Order order;
        order.market_id = pair.market_id;
        order.token_id = leg.token_id;
        order.side = leg.side;
        order.price = leg.price;
        order.original_size = leg.size;
        order.order_type = OrderType::IOC;  // Immediate or Cancel for speed

        auto response = client_->submit_order(order);
        if (!response.success) {
            spdlog::error("Failed to submit {} leg: {}", is_leg1 ? "leg1" : "leg2", response.error);
            return false;
        }

        leg.order_id = response.order_id;
        leg.submit_time = wall_now();
        leg.state = OrderState::SENT;

        spdlog::debug("Submitted {} leg: order_id={}", is_leg1 ? "leg1" : "leg2", leg.order_id);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("Exception submitting leg: {}", e.what());
        return false;
    }
}

bool PairedExecutor::wait_for_fill(PairedOrderV2& pair, PairedOrderV2::Leg& leg, std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        // Query order state
        auto order_state = client_->get_order(leg.order_id);
        if (!order_state) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (order_state->filled_size >= leg.size * 0.99) {
            // Filled!
            leg.filled_size = order_state->filled_size;
            leg.avg_fill_price = order_state->avg_fill_price;
            leg.state = OrderState::FILLED;
            leg.fill_time = wall_now();

            // Invoke fill callback
            if (on_fill_) {
                Fill fill;
                fill.order_id = leg.order_id;
                fill.market_id = pair.market_id;
                fill.token_id = leg.token_id;
                fill.side = leg.side;
                fill.price = leg.avg_fill_price;
                fill.size = leg.filled_size;
                fill.fill_time = leg.fill_time;
                on_fill_(pair, fill);
            }

            return true;
        }

        if (order_state->state == OrderState::CANCELED ||
            order_state->state == OrderState::REJECTED ||
            order_state->state == OrderState::EXPIRED) {
            // Order won't fill
            leg.state = order_state->state;
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return false;  // Timeout
}

bool PairedExecutor::retry_leg(PairedOrderV2& pair, PairedOrderV2::Leg& leg, double current_edge) {
    leg.retry_count++;

    // Calculate adjusted price
    double adjusted_price = calculate_adjusted_price(leg.price, leg.side, leg.retry_count);

    // Check if we still have edge after adjustment
    double price_diff = std::abs(adjusted_price - leg.price);
    double remaining_edge = current_edge - price_diff;

    if (remaining_edge < config_.min_edge_after_adjustment / 100.0) {
        spdlog::warn("Edge too small after adjustment ({:.4f}), giving up", remaining_edge);
        return false;
    }

    // Check max adjustment
    double adjustment_bps = (price_diff / leg.price) * 10000;
    if (adjustment_bps > config_.max_price_adjustment_bps) {
        spdlog::warn("Max price adjustment exceeded ({:.1f}bps), giving up", adjustment_bps);
        return false;
    }

    // Submit new order with adjusted price
    leg.price = adjusted_price;
    return submit_leg(pair, leg, false);
}

bool PairedExecutor::attempt_unwind(PairedOrderV2& pair) {
    unwind_attempts_++;
    spdlog::warn("Attempting to unwind pair {}", pair.pair_id);

    update_pair_state(pair, PairState::UNWIND_PENDING);

    // Determine which leg to unwind
    auto& filled_leg = (pair.leg1.filled_size > 0) ? pair.leg1 : pair.leg2;

    // Calculate unwind price (opposite side, accept slippage)
    Side unwind_side = (filled_leg.side == Side::BUY) ? Side::SELL : Side::BUY;
    double unwind_price = calculate_unwind_price(filled_leg.avg_fill_price, filled_leg.side);

    // Submit unwind order
    Order unwind_order;
    unwind_order.market_id = pair.market_id;
    unwind_order.token_id = filled_leg.token_id;
    unwind_order.side = unwind_side;
    unwind_order.price = unwind_price;
    unwind_order.original_size = filled_leg.filled_size;
    unwind_order.order_type = OrderType::IOC;

    auto response = client_->submit_order(unwind_order);
    if (!response.success) {
        spdlog::error("Failed to submit unwind order: {}", response.error);
        update_pair_state(pair, PairState::ABANDONED);

        if (on_unwind_) {
            on_unwind_(pair, false);
        }
        return false;
    }

    // Wait for unwind fill
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + config_.unwind_timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        auto order_state = client_->get_order(response.order_id);
        if (!order_state) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (order_state->filled_size >= unwind_order.original_size * 0.95) {
            // Unwound!
            update_pair_state(pair, PairState::UNWOUND);

            // Calculate realized loss
            pair.realized_pnl = (order_state->avg_fill_price - filled_leg.avg_fill_price) *
                               filled_leg.filled_size *
                               (filled_leg.side == Side::BUY ? -1 : 1);

            spdlog::info("Unwind successful: realized_pnl=${:.4f}", pair.realized_pnl);

            if (on_unwind_) {
                on_unwind_(pair, true);
            }
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Unwind failed
    spdlog::error("Unwind timeout for pair {}", pair.pair_id);
    update_pair_state(pair, PairState::ABANDONED);

    if (on_unwind_) {
        on_unwind_(pair, false);
    }
    return false;
}

double PairedExecutor::calculate_adjusted_price(double original_price, Side side, int retry_count) {
    double adjustment = original_price * (config_.retry_price_adjustment_bps / 10000.0) * retry_count;

    // For BUY, increase price (more aggressive)
    // For SELL, decrease price (more aggressive)
    if (side == Side::BUY) {
        return original_price + adjustment;
    } else {
        return original_price - adjustment;
    }
}

double PairedExecutor::calculate_unwind_price(double entry_price, Side entry_side) {
    double discount = entry_price * (config_.unwind_price_discount_bps / 10000.0);

    // If we bought, we need to sell - accept lower price
    // If we sold, we need to buy - accept higher price
    if (entry_side == Side::BUY) {
        return entry_price - discount;
    } else {
        return entry_price + discount;
    }
}

void PairedExecutor::update_pair_state(PairedOrderV2& pair, PairState new_state) {
    pair.state = new_state;
    pair.last_update = wall_now();

    spdlog::debug("Pair {} state -> {}", pair.pair_id, pair_state_to_string(new_state));

    // Update stored pair
    {
        std::lock_guard<std::mutex> lock(pairs_mutex_);
        if (pairs_.find(pair.pair_id) != pairs_.end()) {
            pairs_[pair.pair_id] = pair;
        }
    }
}

void PairedExecutor::check_kill_switch_trigger(const PairedOrderV2& pair) {
    if (!kill_switch_) return;

    double unhedged = pair.unhedged_exposure();
    if (unhedged > 0) {
        spdlog::critical("UNHEDGED EXPOSURE: ${:.2f} from pair {}", unhedged, pair.pair_id);

        kill_switch_->activate(
            KillReason::UNHEDGED_POSITION,
            fmt::format("Unhedged exposure ${:.2f} from pair {}", unhedged, pair.pair_id)
        );
    }
}

} // namespace arb
