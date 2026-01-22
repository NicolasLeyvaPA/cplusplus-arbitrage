#include "strategy/strategy_base.hpp"
#include <spdlog/spdlog.h>
#include <cmath>

namespace arb {

StrategyBase::StrategyBase(const std::string& name, const StrategyConfig& config)
    : name_(name)
    , config_(config)
{
}

// ============================================================================
// UnderpricingStrategy (S2) Implementation
// ============================================================================

UnderpricingStrategy::UnderpricingStrategy(const StrategyConfig& config)
    : StrategyBase("S2_Underpricing", config)
{
    spdlog::info("UnderpricingStrategy initialized with min_edge_cents={}", config.min_edge_cents);
}

double UnderpricingStrategy::calculate_edge(double yes_ask, double no_ask, double fee_rate_bps) const {
    // In a binary market, if we buy YES at yes_ask and NO at no_ask,
    // we pay: yes_ask + no_ask
    // We receive: 1.0 (guaranteed, one side settles to $1)
    // Fee on winnings: fee_rate_bps / 10000 on the $1 payout

    double total_cost = yes_ask + no_ask;
    double payout = 1.0;

    // Fee is applied to winnings, which is (payout - losing_side_cost)
    // Simplification: max fee would be on full payout
    double fee = payout * (fee_rate_bps / 10000.0);

    double net_payout = payout - fee;
    double edge = net_payout - total_cost;

    // Convert to cents (multiply by 100)
    return edge * 100.0;
}

bool UnderpricingStrategy::is_profitable(double edge_cents) const {
    return edge_cents >= config_.min_edge_cents;
}

std::vector<Signal> UnderpricingStrategy::evaluate(
    const BinaryMarketBook& book,
    const BtcPrice& btc_price,
    Timestamp now_time)
{
    std::vector<Signal> signals;

    if (!enabled_) return signals;

    // Check if we have liquidity on both sides
    if (!book.has_liquidity()) {
        return signals;
    }

    auto yes_ask = book.yes_book().best_ask();
    auto no_ask = book.no_book().best_ask();

    if (!yes_ask || !no_ask) return signals;

    // Calculate edge
    double edge_cents = calculate_edge(yes_ask->price, no_ask->price, POLYMARKET_FEE_BPS);

    // Check spread constraints
    double yes_spread = book.yes_book().spread();
    double no_spread = book.no_book().spread();

    if (yes_spread > config_.max_spread_to_trade || no_spread > config_.max_spread_to_trade) {
        spdlog::debug("S2: Spread too wide - YES: {:.4f}, NO: {:.4f}", yes_spread, no_spread);
        return signals;
    }

    if (is_profitable(edge_cents)) {
        // Determine size based on available liquidity
        Size max_size = std::min(yes_ask->size, no_ask->size);

        // Create signals for both sides
        Signal yes_signal;
        yes_signal.strategy_name = name_;
        yes_signal.market_id = book.market_id();
        yes_signal.token_id = book.yes_book().symbol();
        yes_signal.side = Side::BUY;
        yes_signal.target_price = yes_ask->price;
        yes_signal.target_size = max_size;
        yes_signal.expected_edge = edge_cents;
        yes_signal.confidence = std::min(1.0, edge_cents / 10.0);  // Higher edge = higher confidence
        yes_signal.generated_at = now_time;
        yes_signal.reason = fmt::format("Sum of asks = {:.4f} + {:.4f} = {:.4f} < 1.0, edge = {:.2f} cents",
                                        yes_ask->price, no_ask->price,
                                        yes_ask->price + no_ask->price, edge_cents);

        Signal no_signal;
        no_signal.strategy_name = name_;
        no_signal.market_id = book.market_id();
        no_signal.token_id = book.no_book().symbol();
        no_signal.side = Side::BUY;
        no_signal.target_price = no_ask->price;
        no_signal.target_size = max_size;
        no_signal.expected_edge = edge_cents;
        no_signal.confidence = yes_signal.confidence;
        no_signal.generated_at = now_time;
        no_signal.reason = yes_signal.reason;

        signals.push_back(yes_signal);
        signals.push_back(no_signal);

        signals_generated_ += 2;

        spdlog::info("S2 Signal: {} - Edge: {:.2f} cents, Confidence: {:.2f}",
                     yes_signal.reason, edge_cents, yes_signal.confidence);
    }

    return signals;
}

// ============================================================================
// StaleOddsStrategy (S1) Implementation
// ============================================================================

StaleOddsStrategy::StaleOddsStrategy(const StrategyConfig& config)
    : StrategyBase("S1_StaleOdds", config)
    , reference_time_(now())
{
    spdlog::info("StaleOddsStrategy initialized with lag_threshold={}bps, staleness_window={}ms",
                 config.lag_move_threshold_bps, config.staleness_window_ms);
}

void StaleOddsStrategy::update_btc_reference(const BtcPrice& price) {
    // Store history for staleness detection
    btc_history_.push_back({price.mid, price.timestamp});

    if (btc_history_.size() > MAX_HISTORY) {
        btc_history_.erase(btc_history_.begin());
    }

    reference_btc_ = price;
    reference_time_ = price.timestamp;
}

double StaleOddsStrategy::detect_btc_move_bps() const {
    if (btc_history_.size() < 2) return 0.0;

    // Calculate move from N samples ago
    size_t lookback = std::min(btc_history_.size(), static_cast<size_t>(10));
    Price old_price = btc_history_[btc_history_.size() - lookback].btc_price;
    Price new_price = btc_history_.back().btc_price;

    if (old_price <= 0) return 0.0;

    double move = (new_price - old_price) / old_price * 10000.0;  // Basis points
    return move;
}

bool StaleOddsStrategy::is_market_stale(const BinaryMarketBook& book, Timestamp now_time) const {
    Duration staleness_threshold = std::chrono::milliseconds(config_.staleness_window_ms);
    return book.is_stale(staleness_threshold);
}

double StaleOddsStrategy::calculate_implied_prob(double yes_ask, double no_ask) const {
    // Simple implied probability from prices
    // If yes_ask = 0.60 and no_ask = 0.45, implied yes_prob ~= 0.60 / (0.60 + 0.45) = 0.57
    double total = yes_ask + no_ask;
    if (total <= 0) return 0.5;
    return yes_ask / total;
}

double StaleOddsStrategy::calculate_expected_prob(double btc_move_bps, double current_implied) const {
    // Simple model: BTC move affects the probability of "up" outcome
    // If BTC moved up significantly, YES (up) probability should increase

    // Sensitivity: how much does 100bps move affect probability?
    // This is a simplified model - in practice would need historical calibration
    double sensitivity = 0.01;  // 1% probability shift per 100bps

    double prob_shift = (btc_move_bps / 100.0) * sensitivity;
    double expected = current_implied + prob_shift;

    // Clamp to [0.05, 0.95]
    return std::max(0.05, std::min(0.95, expected));
}

std::vector<Signal> StaleOddsStrategy::evaluate(
    const BinaryMarketBook& book,
    const BtcPrice& btc_price,
    Timestamp now_time)
{
    std::vector<Signal> signals;

    if (!enabled_) return signals;

    // Update BTC reference
    update_btc_reference(btc_price);

    // Need sufficient history
    if (btc_history_.size() < 5) {
        return signals;
    }

    // Check if market book is stale
    if (!is_market_stale(book, now_time)) {
        // Market is fresh, no staleness arbitrage opportunity
        return signals;
    }

    if (!book.has_liquidity()) {
        return signals;
    }

    auto yes_ask = book.yes_book().best_ask();
    auto no_ask = book.no_book().best_ask();
    auto yes_bid = book.yes_book().best_bid();
    auto no_bid = book.no_book().best_bid();

    if (!yes_ask || !no_ask || !yes_bid || !no_bid) return signals;

    // Detect BTC move
    double btc_move_bps = detect_btc_move_bps();

    // Check if move is significant enough
    if (std::abs(btc_move_bps) < config_.lag_move_threshold_bps) {
        return signals;
    }

    // Calculate implied vs expected probability
    double current_implied_yes = calculate_implied_prob(yes_ask->price, no_ask->price);
    double expected_yes = calculate_expected_prob(btc_move_bps, current_implied_yes);

    double prob_diff = expected_yes - current_implied_yes;

    // Determine trade direction
    // If BTC moved up and market hasn't adjusted -> buy YES
    // If BTC moved down and market hasn't adjusted -> buy NO

    if (std::abs(prob_diff) < 0.02) {  // Need at least 2% probability difference
        return signals;
    }

    Signal signal;
    signal.strategy_name = name_;
    signal.market_id = book.market_id();
    signal.generated_at = now_time;
    signal.confidence = std::min(1.0, std::abs(prob_diff) / 0.10);  // Scale to confidence

    if (prob_diff > 0.02) {
        // Expected YES probability higher than market implies -> buy YES
        signal.token_id = book.yes_book().symbol();
        signal.side = Side::BUY;
        signal.target_price = yes_ask->price;
        signal.target_size = yes_ask->size;
        signal.expected_edge = prob_diff * 100.0;  // Convert to cents per dollar
        signal.reason = fmt::format("BTC moved +{:.1f}bps, market stale. Expected YES={:.2f}, Implied={:.2f}",
                                    btc_move_bps, expected_yes, current_implied_yes);
    } else if (prob_diff < -0.02) {
        // Expected YES probability lower than market implies -> buy NO
        signal.token_id = book.no_book().symbol();
        signal.side = Side::BUY;
        signal.target_price = no_ask->price;
        signal.target_size = no_ask->size;
        signal.expected_edge = -prob_diff * 100.0;
        signal.reason = fmt::format("BTC moved {:.1f}bps, market stale. Expected NO higher, Implied YES={:.2f}",
                                    btc_move_bps, current_implied_yes);
    }

    if (signal.confidence >= config_.min_confidence) {
        signals.push_back(signal);
        signals_generated_++;

        spdlog::info("S1 Signal: {} - Confidence: {:.2f}", signal.reason, signal.confidence);
    }

    return signals;
}

// ============================================================================
// MarketMakingStrategy (S3) Implementation
// ============================================================================

MarketMakingStrategy::MarketMakingStrategy(const StrategyConfig& config)
    : StrategyBase("S3_MarketMaking", config)
{
    spdlog::info("MarketMakingStrategy initialized (disabled by default)");
    enabled_ = false;  // Disabled by default as per requirements
}

double MarketMakingStrategy::calculate_fair_value(const BinaryMarketBook& book,
                                                   const BtcPrice& btc_price) const {
    // Use mid price as simple fair value estimate
    return book.yes_implied_probability();
}

std::pair<Price, Price> MarketMakingStrategy::calculate_quotes(double fair_value, double spread) const {
    double half_spread = spread / 2.0;
    Price bid = fair_value - half_spread;
    Price ask = fair_value + half_spread;

    // Clamp to valid price range
    bid = std::max(0.01, std::min(0.99, bid));
    ask = std::max(0.01, std::min(0.99, ask));

    return {bid, ask};
}

std::vector<Signal> MarketMakingStrategy::evaluate(
    const BinaryMarketBook& book,
    const BtcPrice& btc_price,
    Timestamp now_time)
{
    std::vector<Signal> signals;

    if (!enabled_) return signals;

    // Market making requires careful inventory management
    // This is a conservative implementation

    if (!book.has_liquidity()) {
        return signals;
    }

    double fair_value = calculate_fair_value(book, btc_price);
    double target_spread = 0.02;  // 2% spread minimum

    auto [bid_price, ask_price] = calculate_quotes(fair_value, target_spread);

    // Only quote if spread is reasonable
    double market_spread = book.yes_book().spread();
    if (market_spread < target_spread) {
        // Market spread is tighter than our target, don't compete
        return signals;
    }

    // Generate bid signal
    Signal bid_signal;
    bid_signal.strategy_name = name_;
    bid_signal.market_id = book.market_id();
    bid_signal.token_id = book.yes_book().symbol();
    bid_signal.side = Side::BUY;
    bid_signal.target_price = bid_price;
    bid_signal.target_size = 1.0;  // Minimum size
    bid_signal.expected_edge = target_spread * 50.0;  // Half spread capture
    bid_signal.confidence = 0.5;
    bid_signal.generated_at = now_time;
    bid_signal.reason = "Market making bid";

    // Generate ask signal
    Signal ask_signal;
    ask_signal.strategy_name = name_;
    ask_signal.market_id = book.market_id();
    ask_signal.token_id = book.yes_book().symbol();
    ask_signal.side = Side::SELL;
    ask_signal.target_price = ask_price;
    ask_signal.target_size = 1.0;
    ask_signal.expected_edge = target_spread * 50.0;
    ask_signal.confidence = 0.5;
    ask_signal.generated_at = now_time;
    ask_signal.reason = "Market making ask";

    signals.push_back(bid_signal);
    signals.push_back(ask_signal);

    signals_generated_ += 2;

    return signals;
}

} // namespace arb
