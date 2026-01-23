#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <chrono>
#include <memory>
#include <optional>
#include <cmath>

#include "persistence/session_database.hpp"
#include "arbitrage/funding_settlement_engine.hpp"

namespace arb {

// ============================================================================
// DELTA-NEUTRAL ENFORCER
//
// Ensures positions remain delta-neutral across exchanges and implements
// kill conditions for emergency position closure.
//
// Based on empirical analysis of constraint arbitrage failure modes:
// - Oct 2025 crash: $19B liquidations, $65B OI wipeout
// - USDe de-peg to $0.60 on Binance triggered unified margin contagion
// - ADL (Auto-Deleveraging) converted hedged positions to naked longs
// - Infrastructure lock-ups prevented manual intervention
//
// Kill Conditions (8 total):
// 1. BASIS_DIVERGENCE: Cross-exchange basis exceeds threshold
// 2. FUNDING_FLIP: Direction of net funding reverses
// 3. SPREAD_COLLAPSE: Funding spread falls below min threshold
// 4. EXCHANGE_HALT: Exchange becomes unreachable
// 5. DRAWDOWN_LIMIT: Session drawdown exceeds configured limit
// 6. ADL_RISK: Auto-deleveraging queue priority too high
// 7. STABLECOIN_DEPEG: Collateral asset de-pegging (unified margin contagion)
// 8. LIQUIDATION_CASCADE: Extreme leverage/OI skew detected
// ============================================================================

enum class KillReason {
    BASIS_DIVERGENCE,
    FUNDING_FLIP,
    SPREAD_COLLAPSE,
    EXCHANGE_HALT,
    DRAWDOWN_LIMIT,
    ADL_RISK,              // Auto-deleveraging imminent
    STABLECOIN_DEPEG,      // Collateral de-peg (unified margin contagion)
    LIQUIDATION_CASCADE    // Market-wide liquidation event
};

std::string to_string(KillReason reason);

struct DeltaNeutralConfig {
    // Delta limits
    double max_delta_notional{500};        // Max net delta in USD
    double delta_hedge_threshold{200};     // Hedge when delta exceeds this

    // Kill condition thresholds (original 5)
    double basis_divergence_limit{0.005};  // 0.5% basis divergence
    double basis_divergence_warning{0.003};// 0.3% warning threshold
    double min_funding_spread{0.00005};    // 0.005% minimum spread
    double max_session_drawdown{0.03};     // 3% max drawdown
    double funding_flip_buffer{0.00002};   // Buffer before flip triggers kill

    // NEW: ADL risk thresholds (from Oct 2025 crash analysis)
    double adl_queue_warning{0.7};         // 70% up ADL queue = warning
    double adl_queue_critical{0.9};        // 90% up ADL queue = kill
    double insurance_fund_warning{0.3};    // Warn if fund < 30% of normal

    // NEW: Stablecoin de-peg thresholds (unified margin contagion)
    double stablecoin_depeg_warning{0.005}; // 0.5% off peg = warning
    double stablecoin_depeg_critical{0.02}; // 2% off peg = kill (USDe hit 40% in Oct 2025)

    // NEW: Liquidation cascade detection
    double leverage_ratio_warning{0.7};    // 70% of max leverage = warning
    double oi_skew_critical{0.8};          // 80%+ long/short skew = danger
    double liquidation_volume_spike{3.0};  // 3x normal liq volume = cascade

    // Timing
    int64_t exchange_timeout_ms{30000};    // 30s timeout = halt
    int64_t heartbeat_interval_ms{5000};   // Expected heartbeat frequency
    int64_t api_latency_warning_ms{2000};  // >2s latency = infrastructure stress

    // Hedge parameters
    double hedge_aggressiveness{1.2};      // Overshoot hedge by 20%
    int max_hedge_retries{3};

    // Regime-aware sizing (from research)
    bool enable_dynamic_sizing{true};      // Shrink positions in high-vol
    double vol_scaling_factor{0.5};        // Half size at 2x normal vol
};

struct DeltaSnapshot {
    int64_t timestamp{0};
    double total_long_notional{0};
    double total_short_notional{0};
    double net_delta{0};                   // long - short (positive = net long)
    double net_delta_pct{0};               // net_delta / total_exposure
    std::map<std::string, double> delta_by_venue;
    bool is_neutral{false};
};

struct BasisSnapshot {
    int64_t timestamp{0};
    std::map<std::string, double> mark_price_by_venue;
    std::map<std::string, double> index_price_by_venue;
    double max_basis_divergence{0};
    std::string high_basis_venue;
    std::string low_basis_venue;
    double basis_spread{0};                // high - low
    bool is_warning{false};
    bool is_critical{false};
};

struct HedgeOrder {
    std::string venue;
    std::string instrument;
    OrderSide side;
    double qty;
    double urgency;                        // 0-1, higher = more urgent
    std::string reason;
};

struct KillSignal {
    bool should_kill{false};
    KillReason reason;
    std::string detail;
    double urgency;                        // 0-1, higher = close faster
    double estimated_loss;                 // Expected loss from emergency close
};

class DeltaNeutralEnforcer {
public:
    explicit DeltaNeutralEnforcer(
        std::shared_ptr<SessionDatabase> db,
        const std::string& session_id,
        const DeltaNeutralConfig& config = DeltaNeutralConfig()
    );

    // ========================================================================
    // Position Updates
    // ========================================================================

    // Update position for a venue
    void update_position(const Position& position);

    // Update mark price for a venue
    void update_mark_price(const std::string& venue, const std::string& instrument,
                          double mark_price, double index_price);

    // Record exchange heartbeat
    void record_heartbeat(const std::string& venue);

    // Update session equity for drawdown tracking
    void update_equity(double equity);

    // ========================================================================
    // Delta Analysis
    // ========================================================================

    // Compute current delta snapshot
    DeltaSnapshot compute_delta() const;

    // Check if rebalancing is needed
    std::vector<HedgeOrder> check_hedge_needed();

    // ========================================================================
    // Basis Monitoring
    // ========================================================================

    // Compute current basis snapshot
    BasisSnapshot compute_basis() const;

    // ========================================================================
    // Kill Condition Checking
    // ========================================================================

    // Check all kill conditions
    // Returns the most critical kill signal if any condition is triggered
    std::optional<KillSignal> check_kill_conditions();

    // Individual kill condition checks (original 5)
    std::optional<KillSignal> check_basis_divergence();
    std::optional<KillSignal> check_funding_flip();
    std::optional<KillSignal> check_spread_collapse();
    std::optional<KillSignal> check_exchange_halt();
    std::optional<KillSignal> check_drawdown_limit();

    // NEW kill condition checks (from Oct 2025 crash analysis)
    std::optional<KillSignal> check_adl_risk();
    std::optional<KillSignal> check_stablecoin_depeg();
    std::optional<KillSignal> check_liquidation_cascade();

    // ========================================================================
    // NEW: Market State Updates (for advanced kill conditions)
    // ========================================================================

    // Update ADL queue position (0-1, where 1 = top of queue = imminent ADL)
    void update_adl_position(const std::string& venue, double queue_position);

    // Update stablecoin prices for de-peg detection
    void update_stablecoin_price(const std::string& symbol, double price);

    // Update market-wide metrics for cascade detection
    void update_market_metrics(double total_oi, double long_oi, double short_oi,
                               double liquidation_volume_24h, double avg_liq_volume);

    // ========================================================================
    // Kill Execution
    // ========================================================================

    // Generate orders to close all positions
    std::vector<HedgeOrder> generate_kill_orders();

    // Record kill event
    void record_kill(const KillSignal& signal, const std::vector<Position>& closed_positions);

    // ========================================================================
    // Funding Spread Tracking
    // ========================================================================

    // Update funding rate for a venue
    void update_funding_rate(const std::string& venue, double rate);

    // Get current funding spread (max - min)
    double get_funding_spread() const;

    // Get direction of net funding (positive = receiving net funding)
    double get_net_funding_direction() const;

    // ========================================================================
    // Callbacks
    // ========================================================================

    using HedgeCallback = std::function<void(const HedgeOrder&)>;
    using KillCallback = std::function<void(const KillSignal&)>;
    using WarningCallback = std::function<void(const std::string&, double urgency)>;

    void set_hedge_callback(HedgeCallback cb) { on_hedge_ = std::move(cb); }
    void set_kill_callback(KillCallback cb) { on_kill_ = std::move(cb); }
    void set_warning_callback(WarningCallback cb) { on_warning_ = std::move(cb); }

    // ========================================================================
    // Statistics
    // ========================================================================

    struct Stats {
        int delta_checks{0};
        int hedges_triggered{0};
        int kill_signals{0};
        int warnings_issued{0};
        double max_delta_seen{0};
        double max_basis_divergence_seen{0};
        double max_drawdown_seen{0};
        int64_t last_all_venues_healthy{0};
    };

    Stats stats() const { return stats_; }

    // Get current state
    const DeltaNeutralConfig& config() const { return config_; }

private:
    std::shared_ptr<SessionDatabase> db_;
    std::string session_id_;
    DeltaNeutralConfig config_;
    Stats stats_;

    // Current positions by venue:instrument
    std::map<std::string, Position> positions_;

    // Mark prices by venue:instrument
    std::map<std::string, double> mark_prices_;
    std::map<std::string, double> index_prices_;

    // Funding rates by venue
    std::map<std::string, double> funding_rates_;

    // Exchange health
    std::map<std::string, int64_t> last_heartbeat_;

    // Session tracking
    double starting_equity_{0};
    double current_equity_{0};
    double high_water_mark_{0};
    double initial_funding_direction_{0};
    bool funding_direction_set_{false};

    // NEW: ADL tracking (from Oct 2025 crash analysis)
    std::map<std::string, double> adl_queue_positions_;  // venue -> 0-1 queue position

    // NEW: Stablecoin prices for de-peg detection
    std::map<std::string, double> stablecoin_prices_;  // USDT, USDC, USDe, etc.

    // NEW: Market-wide metrics for cascade detection
    double total_oi_{0};
    double long_oi_{0};
    double short_oi_{0};
    double liquidation_volume_24h_{0};
    double avg_liquidation_volume_{0};

    // Callbacks
    HedgeCallback on_hedge_;
    KillCallback on_kill_;
    WarningCallback on_warning_;

    // Helper functions
    std::string make_key(const std::string& venue, const std::string& instrument) const;
};

// ============================================================================
// Implementation
// ============================================================================

inline std::string to_string(KillReason reason) {
    switch (reason) {
        case KillReason::BASIS_DIVERGENCE: return "BASIS_DIVERGENCE";
        case KillReason::FUNDING_FLIP: return "FUNDING_FLIP";
        case KillReason::SPREAD_COLLAPSE: return "SPREAD_COLLAPSE";
        case KillReason::EXCHANGE_HALT: return "EXCHANGE_HALT";
        case KillReason::DRAWDOWN_LIMIT: return "DRAWDOWN_LIMIT";
        case KillReason::ADL_RISK: return "ADL_RISK";
        case KillReason::STABLECOIN_DEPEG: return "STABLECOIN_DEPEG";
        case KillReason::LIQUIDATION_CASCADE: return "LIQUIDATION_CASCADE";
    }
    return "UNKNOWN";
}

inline DeltaNeutralEnforcer::DeltaNeutralEnforcer(
    std::shared_ptr<SessionDatabase> db,
    const std::string& session_id,
    const DeltaNeutralConfig& config
)
    : db_(std::move(db))
    , session_id_(session_id)
    , config_(config)
{
}

inline std::string DeltaNeutralEnforcer::make_key(
    const std::string& venue,
    const std::string& instrument
) const {
    return venue + ":" + instrument;
}

inline void DeltaNeutralEnforcer::update_position(const Position& position) {
    std::string key = make_key(position.venue, position.instrument);
    positions_[key] = position;
}

inline void DeltaNeutralEnforcer::update_mark_price(
    const std::string& venue,
    const std::string& instrument,
    double mark_price,
    double index_price
) {
    std::string key = make_key(venue, instrument);
    mark_prices_[key] = mark_price;
    index_prices_[key] = index_price;
}

inline void DeltaNeutralEnforcer::record_heartbeat(const std::string& venue) {
    last_heartbeat_[venue] = now_micros();
}

inline void DeltaNeutralEnforcer::update_equity(double equity) {
    if (starting_equity_ == 0) {
        starting_equity_ = equity;
        high_water_mark_ = equity;
    }
    current_equity_ = equity;
    if (equity > high_water_mark_) {
        high_water_mark_ = equity;
    }
}

inline void DeltaNeutralEnforcer::update_funding_rate(const std::string& venue, double rate) {
    funding_rates_[venue] = rate;
}

inline DeltaSnapshot DeltaNeutralEnforcer::compute_delta() const {
    DeltaSnapshot snap;
    snap.timestamp = now_micros();

    for (const auto& [key, pos] : positions_) {
        double mark = pos.mark_price;
        auto it = mark_prices_.find(key);
        if (it != mark_prices_.end() && it->second > 0) {
            mark = it->second;
        }
        if (mark <= 0) mark = pos.avg_price;

        double notional = pos.qty * mark;
        snap.delta_by_venue[pos.venue] = notional;

        if (pos.qty > 0) {
            snap.total_long_notional += notional;
        } else {
            snap.total_short_notional += std::abs(notional);
        }
    }

    snap.net_delta = snap.total_long_notional - snap.total_short_notional;
    double total_exposure = snap.total_long_notional + snap.total_short_notional;
    snap.net_delta_pct = (total_exposure > 0) ? snap.net_delta / total_exposure : 0;
    snap.is_neutral = std::abs(snap.net_delta) <= config_.max_delta_notional;

    return snap;
}

inline std::vector<HedgeOrder> DeltaNeutralEnforcer::check_hedge_needed() {
    std::vector<HedgeOrder> orders;
    stats_.delta_checks++;

    auto delta = compute_delta();
    stats_.max_delta_seen = std::max(stats_.max_delta_seen, std::abs(delta.net_delta));

    if (std::abs(delta.net_delta) <= config_.delta_hedge_threshold) {
        return orders;  // No hedge needed
    }

    stats_.hedges_triggered++;

    // Determine which venue to hedge on
    // Strategy: reduce position on the side with larger exposure
    double hedge_amount = delta.net_delta * config_.hedge_aggressiveness;

    // Find the venue with the largest position in the direction we need to reduce
    std::string hedge_venue;
    double max_pos = 0;

    for (const auto& [key, pos] : positions_) {
        if ((hedge_amount > 0 && pos.qty > max_pos) ||
            (hedge_amount < 0 && pos.qty < max_pos)) {
            max_pos = pos.qty;
            hedge_venue = pos.venue;
        }
    }

    if (!hedge_venue.empty()) {
        HedgeOrder order;
        order.venue = hedge_venue;
        // Find instrument for this venue
        for (const auto& [key, pos] : positions_) {
            if (pos.venue == hedge_venue) {
                order.instrument = pos.instrument;
                break;
            }
        }
        order.side = (hedge_amount > 0) ? OrderSide::SELL : OrderSide::BUY;
        order.qty = std::abs(hedge_amount) / (mark_prices_.empty() ? 1.0 :
            mark_prices_.begin()->second);
        order.urgency = std::min(1.0, std::abs(delta.net_delta) / config_.max_delta_notional);
        order.reason = "Delta hedge: net delta = $" + std::to_string(delta.net_delta);

        orders.push_back(order);

        if (on_hedge_) {
            on_hedge_(order);
        }
    }

    return orders;
}

inline BasisSnapshot DeltaNeutralEnforcer::compute_basis() const {
    BasisSnapshot snap;
    snap.timestamp = now_micros();

    double max_basis = -1e9;
    double min_basis = 1e9;

    for (const auto& [key, mark] : mark_prices_) {
        auto index_it = index_prices_.find(key);
        if (index_it == index_prices_.end()) continue;

        double index = index_it->second;
        if (index <= 0) continue;

        double basis = (mark - index) / index;

        // Parse venue from key
        size_t colon = key.find(':');
        std::string venue = (colon != std::string::npos) ? key.substr(0, colon) : key;

        snap.mark_price_by_venue[venue] = mark;
        snap.index_price_by_venue[venue] = index;

        if (basis > max_basis) {
            max_basis = basis;
            snap.high_basis_venue = venue;
        }
        if (basis < min_basis) {
            min_basis = basis;
            snap.low_basis_venue = venue;
        }
    }

    if (max_basis > -1e8 && min_basis < 1e8) {
        snap.max_basis_divergence = max_basis - min_basis;
        snap.basis_spread = snap.max_basis_divergence;
        snap.is_warning = snap.max_basis_divergence >= config_.basis_divergence_warning;
        snap.is_critical = snap.max_basis_divergence >= config_.basis_divergence_limit;
    }

    return snap;
}

inline double DeltaNeutralEnforcer::get_funding_spread() const {
    if (funding_rates_.empty()) return 0;

    double max_rate = -1e9;
    double min_rate = 1e9;

    for (const auto& [venue, rate] : funding_rates_) {
        max_rate = std::max(max_rate, rate);
        min_rate = std::min(min_rate, rate);
    }

    return max_rate - min_rate;
}

inline double DeltaNeutralEnforcer::get_net_funding_direction() const {
    // Calculate expected net funding based on positions
    double net = 0;
    for (const auto& [key, pos] : positions_) {
        auto rate_it = funding_rates_.find(pos.venue);
        if (rate_it != funding_rates_.end()) {
            // Short position receives funding when rate > 0
            // Long position pays funding when rate > 0
            net -= pos.qty * rate_it->second;
        }
    }
    return net;
}

inline std::optional<KillSignal> DeltaNeutralEnforcer::check_kill_conditions() {
    // Check all conditions and return the most urgent one
    // Priority order reflects lessons from Oct 2025 crash
    std::optional<KillSignal> worst;

    auto checks = {
        // Original 5 conditions
        check_basis_divergence(),
        check_funding_flip(),
        check_spread_collapse(),
        check_exchange_halt(),
        check_drawdown_limit(),
        // NEW: From Oct 2025 crash analysis
        check_adl_risk(),           // ADL converts hedges to naked positions
        check_stablecoin_depeg(),   // Unified margin contagion (USDe hit $0.60)
        check_liquidation_cascade() // Market-wide deleveraging event
    };

    for (const auto& signal : checks) {
        if (signal && (!worst || signal->urgency > worst->urgency)) {
            worst = signal;
        }
    }

    if (worst) {
        stats_.kill_signals++;
        if (on_kill_) {
            on_kill_(*worst);
        }
    }

    return worst;
}

inline std::optional<KillSignal> DeltaNeutralEnforcer::check_basis_divergence() {
    auto basis = compute_basis();
    stats_.max_basis_divergence_seen = std::max(
        stats_.max_basis_divergence_seen, basis.max_basis_divergence);

    if (basis.is_warning && !basis.is_critical) {
        stats_.warnings_issued++;
        if (on_warning_) {
            on_warning_("Basis divergence warning: " +
                std::to_string(basis.max_basis_divergence * 100) + "%",
                0.5);
        }
    }

    if (!basis.is_critical) {
        return std::nullopt;
    }

    KillSignal signal;
    signal.should_kill = true;
    signal.reason = KillReason::BASIS_DIVERGENCE;
    signal.detail = "Basis divergence " +
        std::to_string(basis.max_basis_divergence * 100) + "% exceeds limit " +
        std::to_string(config_.basis_divergence_limit * 100) + "%";
    signal.urgency = 0.9;
    signal.estimated_loss = basis.max_basis_divergence *
        compute_delta().total_long_notional;

    return signal;
}

inline std::optional<KillSignal> DeltaNeutralEnforcer::check_funding_flip() {
    double current_direction = get_net_funding_direction();

    if (!funding_direction_set_ && std::abs(current_direction) > config_.funding_flip_buffer) {
        initial_funding_direction_ = current_direction;
        funding_direction_set_ = true;
        return std::nullopt;
    }

    if (!funding_direction_set_) {
        return std::nullopt;
    }

    // Check if funding has flipped (was positive, now negative or vice versa)
    bool flipped = (initial_funding_direction_ > 0 && current_direction < -config_.funding_flip_buffer) ||
                   (initial_funding_direction_ < 0 && current_direction > config_.funding_flip_buffer);

    if (!flipped) {
        return std::nullopt;
    }

    KillSignal signal;
    signal.should_kill = true;
    signal.reason = KillReason::FUNDING_FLIP;
    signal.detail = "Net funding flipped from " +
        std::to_string(initial_funding_direction_) + " to " +
        std::to_string(current_direction);
    signal.urgency = 0.7;
    signal.estimated_loss = 0;  // No immediate loss, but strategy no longer profitable

    return signal;
}

inline std::optional<KillSignal> DeltaNeutralEnforcer::check_spread_collapse() {
    double spread = get_funding_spread();

    if (spread >= config_.min_funding_spread) {
        return std::nullopt;
    }

    // Warning before kill
    if (spread >= config_.min_funding_spread * 0.5) {
        stats_.warnings_issued++;
        if (on_warning_) {
            on_warning_("Funding spread collapsing: " +
                std::to_string(spread * 100) + "%", 0.6);
        }
        return std::nullopt;
    }

    KillSignal signal;
    signal.should_kill = true;
    signal.reason = KillReason::SPREAD_COLLAPSE;
    signal.detail = "Funding spread " + std::to_string(spread * 100) +
        "% below minimum " + std::to_string(config_.min_funding_spread * 100) + "%";
    signal.urgency = 0.5;  // Less urgent - can wait for better exit
    signal.estimated_loss = 0;

    return signal;
}

inline std::optional<KillSignal> DeltaNeutralEnforcer::check_exchange_halt() {
    int64_t now = now_micros();
    std::vector<std::string> halted_exchanges;

    for (const auto& [venue, last_hb] : last_heartbeat_) {
        int64_t elapsed_ms = (now - last_hb) / 1000;
        if (elapsed_ms > config_.exchange_timeout_ms) {
            halted_exchanges.push_back(venue);
        }
    }

    if (halted_exchanges.empty()) {
        stats_.last_all_venues_healthy = now;
        return std::nullopt;
    }

    // Warning for single exchange
    if (halted_exchanges.size() == 1) {
        stats_.warnings_issued++;
        if (on_warning_) {
            on_warning_("Exchange timeout: " + halted_exchanges[0], 0.8);
        }
    }

    KillSignal signal;
    signal.should_kill = true;
    signal.reason = KillReason::EXCHANGE_HALT;
    signal.detail = "Exchange(s) halted: ";
    for (size_t i = 0; i < halted_exchanges.size(); i++) {
        if (i > 0) signal.detail += ", ";
        signal.detail += halted_exchanges[i];
    }
    signal.urgency = 0.95;  // Very urgent - can't hedge
    signal.estimated_loss = compute_delta().net_delta;  // Worst case: entire delta exposed

    return signal;
}

inline std::optional<KillSignal> DeltaNeutralEnforcer::check_drawdown_limit() {
    if (high_water_mark_ <= 0 || current_equity_ <= 0) {
        return std::nullopt;
    }

    double drawdown = (high_water_mark_ - current_equity_) / high_water_mark_;
    stats_.max_drawdown_seen = std::max(stats_.max_drawdown_seen, drawdown);

    // Warning at 50% of limit
    if (drawdown >= config_.max_session_drawdown * 0.5 &&
        drawdown < config_.max_session_drawdown) {
        stats_.warnings_issued++;
        if (on_warning_) {
            on_warning_("Drawdown warning: " + std::to_string(drawdown * 100) + "%", 0.6);
        }
    }

    if (drawdown < config_.max_session_drawdown) {
        return std::nullopt;
    }

    KillSignal signal;
    signal.should_kill = true;
    signal.reason = KillReason::DRAWDOWN_LIMIT;
    signal.detail = "Session drawdown " + std::to_string(drawdown * 100) +
        "% exceeds limit " + std::to_string(config_.max_session_drawdown * 100) + "%";
    signal.urgency = 0.8;
    signal.estimated_loss = high_water_mark_ - current_equity_;

    return signal;
}

inline std::vector<HedgeOrder> DeltaNeutralEnforcer::generate_kill_orders() {
    std::vector<HedgeOrder> orders;

    for (const auto& [key, pos] : positions_) {
        if (pos.qty == 0) continue;

        HedgeOrder order;
        order.venue = pos.venue;
        order.instrument = pos.instrument;
        order.side = (pos.qty > 0) ? OrderSide::SELL : OrderSide::BUY;
        order.qty = std::abs(pos.qty);
        order.urgency = 1.0;  // Maximum urgency
        order.reason = "KILL: Close all positions";

        orders.push_back(order);
    }

    return orders;
}

inline void DeltaNeutralEnforcer::record_kill(
    const KillSignal& signal,
    const std::vector<Position>& closed_positions
) {
    if (!db_) return;

    KillEvent event;
    event.session_id = session_id_;
    event.reason_code = to_string(signal.reason);
    event.reason_detail = signal.detail;
    event.timestamp = now_micros();
    event.pnl_impact = signal.estimated_loss;

    // Build JSON of closed positions
    std::string json = "[";
    for (size_t i = 0; i < closed_positions.size(); i++) {
        if (i > 0) json += ",";
        const auto& p = closed_positions[i];
        json += "{\"venue\":\"" + p.venue + "\",\"qty\":" + std::to_string(p.qty) + "}";
    }
    json += "]";
    event.positions_closed_json = json;

    db_->insert_kill_event(event);
}

// ============================================================================
// NEW: Update methods for advanced kill conditions
// ============================================================================

inline void DeltaNeutralEnforcer::update_adl_position(
    const std::string& venue,
    double queue_position
) {
    adl_queue_positions_[venue] = queue_position;
}

inline void DeltaNeutralEnforcer::update_stablecoin_price(
    const std::string& symbol,
    double price
) {
    stablecoin_prices_[symbol] = price;
}

inline void DeltaNeutralEnforcer::update_market_metrics(
    double total_oi,
    double long_oi,
    double short_oi,
    double liquidation_volume_24h,
    double avg_liq_volume
) {
    total_oi_ = total_oi;
    long_oi_ = long_oi;
    short_oi_ = short_oi;
    liquidation_volume_24h_ = liquidation_volume_24h;
    avg_liquidation_volume_ = avg_liq_volume;
}

// ============================================================================
// NEW: Kill condition checks from Oct 2025 crash analysis
// ============================================================================

inline std::optional<KillSignal> DeltaNeutralEnforcer::check_adl_risk() {
    // ADL (Auto-Deleveraging) risk check
    // In Oct 2025, ADL forcibly closed profitable short hedges, leaving
    // traders with naked long exposure during a crash
    //
    // ADL queue position: 0 = safe, 1 = you're next to be ADL'd

    double max_adl_position = 0;
    std::string worst_venue;

    for (const auto& [venue, position] : adl_queue_positions_) {
        if (position > max_adl_position) {
            max_adl_position = position;
            worst_venue = venue;
        }
    }

    // Warning at 70% up the queue
    if (max_adl_position >= config_.adl_queue_warning &&
        max_adl_position < config_.adl_queue_critical) {
        stats_.warnings_issued++;
        if (on_warning_) {
            on_warning_("ADL risk warning on " + worst_venue + ": " +
                std::to_string(max_adl_position * 100) + "% up queue", 0.7);
        }
    }

    if (max_adl_position < config_.adl_queue_critical) {
        return std::nullopt;
    }

    KillSignal signal;
    signal.should_kill = true;
    signal.reason = KillReason::ADL_RISK;
    signal.detail = "ADL imminent on " + worst_venue + ": " +
        std::to_string(max_adl_position * 100) + "% up queue. " +
        "Profitable hedges may be forcibly closed.";
    signal.urgency = 0.95;  // Very high - ADL removes your hedge instantly
    signal.estimated_loss = compute_delta().total_long_notional * 0.1;  // Estimate 10% loss

    return signal;
}

inline std::optional<KillSignal> DeltaNeutralEnforcer::check_stablecoin_depeg() {
    // Stablecoin de-peg check (unified margin contagion)
    // In Oct 2025, USDe traded at ~$0.60 on Binance while pegged elsewhere.
    // This local pricing marked down collateral value, triggering portfolio-wide
    // liquidations for traders who were theoretically hedged.

    double max_depeg = 0;
    std::string worst_stable;

    for (const auto& [symbol, price] : stablecoin_prices_) {
        double depeg = std::abs(1.0 - price);
        if (depeg > max_depeg) {
            max_depeg = depeg;
            worst_stable = symbol;
        }
    }

    // Warning at 0.5% off peg
    if (max_depeg >= config_.stablecoin_depeg_warning &&
        max_depeg < config_.stablecoin_depeg_critical) {
        stats_.warnings_issued++;
        if (on_warning_) {
            on_warning_(worst_stable + " de-peg warning: " +
                std::to_string(max_depeg * 100) + "% off peg", 0.6);
        }
    }

    if (max_depeg < config_.stablecoin_depeg_critical) {
        return std::nullopt;
    }

    KillSignal signal;
    signal.should_kill = true;
    signal.reason = KillReason::STABLECOIN_DEPEG;
    signal.detail = worst_stable + " de-pegged " +
        std::to_string(max_depeg * 100) + "%. " +
        "Unified margin contagion risk - collateral marked down locally.";
    signal.urgency = 0.92;  // Very high - can trigger cascade liquidations
    signal.estimated_loss = current_equity_ * max_depeg;  // Collateral haircut

    return signal;
}

inline std::optional<KillSignal> DeltaNeutralEnforcer::check_liquidation_cascade() {
    // Liquidation cascade detection
    // Oct 2025 saw $19B in liquidations and $65B OI wipeout.
    // High leverage + extreme OI skew + spiking liq volume = cascade

    if (total_oi_ <= 0 || avg_liquidation_volume_ <= 0) {
        return std::nullopt;  // No data
    }

    // Check OI skew (80%+ long = danger in downturn)
    double oi_skew = (total_oi_ > 0) ?
        std::max(long_oi_, short_oi_) / total_oi_ : 0;

    // Check liquidation volume spike
    double liq_ratio = (avg_liquidation_volume_ > 0) ?
        liquidation_volume_24h_ / avg_liquidation_volume_ : 0;

    bool skew_critical = oi_skew >= config_.oi_skew_critical;
    bool liq_spike = liq_ratio >= config_.liquidation_volume_spike;

    // Warning if either condition is elevated
    if ((skew_critical || liq_spike) && !(skew_critical && liq_spike)) {
        stats_.warnings_issued++;
        if (on_warning_) {
            std::string msg = "Cascade risk: ";
            if (skew_critical) msg += "OI skew " + std::to_string(oi_skew * 100) + "%. ";
            if (liq_spike) msg += "Liq volume " + std::to_string(liq_ratio) + "x normal.";
            on_warning_(msg, 0.65);
        }
    }

    // Kill only if BOTH conditions are critical (high skew + liq spike)
    if (!(skew_critical && liq_spike)) {
        return std::nullopt;
    }

    KillSignal signal;
    signal.should_kill = true;
    signal.reason = KillReason::LIQUIDATION_CASCADE;
    signal.detail = "Liquidation cascade detected: OI skew " +
        std::to_string(oi_skew * 100) + "%, liq volume " +
        std::to_string(liq_ratio) + "x normal. Expect 90%+ liquidity loss.";
    signal.urgency = 0.85;
    signal.estimated_loss = current_equity_ * 0.15;  // Estimate 15% slippage in cascade

    return signal;
}

}  // namespace arb
