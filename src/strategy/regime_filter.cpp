#include "strategy/regime_filter.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace arb {

std::string RegimeAssessment::summary() const {
    std::string s = fmt::format("Regime: {} | ", regime_to_string(overall_regime));
    s += fmt::format("Vol:{:.0f}% Liq:{:.0f}% Spd:{:.0f}% Time:{:.0f}% Mom:{:.0f}% | ",
                    volatility_score * 100,
                    liquidity_score * 100,
                    spread_score * 100,
                    time_score * 100,
                    momentum_score * 100);
    s += fmt::format("Size:{:.1f}x Edge:{:.1f}x", size_multiplier, edge_multiplier);

    if (!warnings.empty()) {
        s += " | Warnings: ";
        for (size_t i = 0; i < warnings.size(); ++i) {
            if (i > 0) s += ", ";
            s += warnings[i];
        }
    }

    return s;
}

RegimeFilter::RegimeFilter(const Config& config)
    : config_(config)
{
    spdlog::info("RegimeFilter initialized");
}

void RegimeFilter::update_btc_price(double price, Timestamp time) {
    std::lock_guard<std::mutex> lock(price_mutex_);

    price_history_.push_back({price, time});

    while (price_history_.size() > MAX_PRICE_HISTORY) {
        price_history_.pop_front();
    }
}

void RegimeFilter::update_market_data(const BinaryMarketBook& book) {
    std::lock_guard<std::mutex> lock(book_mutex_);
    book_cache_[book.market_id()] = book;
}

RegimeAssessment RegimeFilter::assess(const BinaryMarketBook& book, Timestamp now) const {
    RegimeAssessment result;

    // Calculate component scores
    result.volatility_score = calculate_volatility_score();
    result.liquidity_score = calculate_liquidity_score(book);
    result.spread_score = calculate_spread_score(book);
    result.time_score = calculate_time_score(book);
    result.momentum_score = calculate_momentum_score();

    // Store raw metrics
    result.current_volatility = calculate_hourly_volatility();
    result.price_momentum = calculate_recent_momentum();

    // Calculate spread
    auto yes_bid = book.yes_book().best_bid();
    auto yes_ask = book.yes_book().best_ask();
    if (yes_bid && yes_ask && yes_ask->price > 0) {
        result.avg_spread_bps = (yes_ask->price - yes_bid->price) / yes_ask->price * 10000;
    }

    // Calculate liquidity
    if (yes_ask) {
        result.total_liquidity_usd = yes_ask->size * yes_ask->price;
    }
    auto no_ask = book.no_book().best_ask();
    if (no_ask) {
        result.total_liquidity_usd += no_ask->size * no_ask->price;
    }

    // Time to expiry
    auto time_diff = book.end_date() - wall_now();
    result.hours_to_expiry = std::chrono::duration_cast<std::chrono::minutes>(time_diff).count() / 60.0;

    // Calculate overall score (weighted average)
    double overall = result.volatility_score * config_.vol_weight +
                    result.liquidity_score * config_.liquidity_weight +
                    result.spread_score * config_.spread_weight +
                    result.time_score * config_.time_weight +
                    result.momentum_score * config_.momentum_weight;

    // Determine regime
    result.overall_regime = score_to_regime(overall);

    // Get adjustments
    auto adj_it = config_.adjustments.find(result.overall_regime);
    if (adj_it != config_.adjustments.end()) {
        result.size_multiplier = adj_it->second.size_mult;
        result.edge_multiplier = adj_it->second.edge_mult;
    }

    // Generate warnings
    if (result.volatility_score < 0.3) {
        result.warnings.push_back(fmt::format("High volatility: {:.2f}%/hr", result.current_volatility * 100));
    }
    if (result.liquidity_score < 0.3) {
        result.warnings.push_back(fmt::format("Low liquidity: ${:.0f}", result.total_liquidity_usd));
    }
    if (result.spread_score < 0.3) {
        result.warnings.push_back(fmt::format("Wide spread: {:.0f}bps", result.avg_spread_bps));
    }
    if (result.time_score < 0.3) {
        result.warnings.push_back(fmt::format("Near expiry: {:.1f}h", result.hours_to_expiry));
    }
    if (std::abs(result.price_momentum) > config_.unfavorable_momentum) {
        result.warnings.push_back(fmt::format("Strong momentum: {:.1f}%", result.price_momentum * 100));
    }

    // Update current regime
    const_cast<RegimeFilter*>(this)->current_regime_.store(result.overall_regime);

    // Record history
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        const_cast<std::deque<RegimeHistory>&>(regime_history_).push_back({
            wall_now(),
            result.overall_regime,
            overall
        });
        while (regime_history_.size() > 1000) {
            const_cast<std::deque<RegimeHistory>&>(regime_history_).pop_front();
        }
    }

    return result;
}

RegimeAssessment RegimeFilter::assess_general() const {
    RegimeAssessment result;

    // Without a specific market, only use volatility and momentum
    result.volatility_score = calculate_volatility_score();
    result.momentum_score = calculate_momentum_score();
    result.liquidity_score = 0.5;  // Neutral
    result.spread_score = 0.5;
    result.time_score = 0.5;

    result.current_volatility = calculate_hourly_volatility();
    result.price_momentum = calculate_recent_momentum();

    double overall = result.volatility_score * 0.6 + result.momentum_score * 0.4;
    result.overall_regime = score_to_regime(overall);

    auto adj_it = config_.adjustments.find(result.overall_regime);
    if (adj_it != config_.adjustments.end()) {
        result.size_multiplier = adj_it->second.size_mult;
        result.edge_multiplier = adj_it->second.edge_mult;
    }

    return result;
}

bool RegimeFilter::should_trade() const {
    MarketRegime regime = current_regime_.load();
    return regime != MarketRegime::DANGEROUS;
}

double RegimeFilter::recommended_size_multiplier() const {
    MarketRegime regime = current_regime_.load();
    auto it = config_.adjustments.find(regime);
    if (it != config_.adjustments.end()) {
        return it->second.size_mult;
    }
    return 1.0;
}

double RegimeFilter::recommended_edge_multiplier() const {
    MarketRegime regime = current_regime_.load();
    auto it = config_.adjustments.find(regime);
    if (it != config_.adjustments.end()) {
        return it->second.edge_mult;
    }
    return 1.0;
}

RegimeFilter::FilterResult RegimeFilter::apply(
    double base_size,
    double min_edge,
    const BinaryMarketBook& book
) const {
    FilterResult result;

    auto assessment = assess(book, now());

    result.adjusted_size = base_size * assessment.size_multiplier;
    result.adjusted_min_edge = min_edge * assessment.edge_multiplier;

    if (assessment.overall_regime == MarketRegime::DANGEROUS) {
        result.should_trade = false;
        result.reason = "DANGEROUS regime: " + assessment.summary();
        result.adjusted_size = 0.0;
    } else {
        result.should_trade = true;
        result.reason = assessment.summary();
    }

    return result;
}

std::vector<RegimeFilter::RegimeHistory> RegimeFilter::get_history(size_t max_entries) const {
    std::lock_guard<std::mutex> lock(history_mutex_);

    std::vector<RegimeHistory> result;
    size_t count = std::min(max_entries, regime_history_.size());

    auto it = regime_history_.end() - count;
    while (it != regime_history_.end()) {
        result.push_back(*it);
        ++it;
    }

    return result;
}

std::string RegimeFilter::status_summary() const {
    auto assessment = assess_general();
    return assessment.summary();
}

double RegimeFilter::calculate_volatility_score() const {
    double vol = calculate_hourly_volatility();
    return score_to_regime_score(vol, config_.favorable_vol, config_.neutral_vol, config_.unfavorable_vol);
}

double RegimeFilter::calculate_liquidity_score(const BinaryMarketBook& book) const {
    double liquidity = 0.0;

    auto yes_ask = book.yes_book().best_ask();
    auto no_ask = book.no_book().best_ask();

    if (yes_ask) {
        liquidity += yes_ask->size * yes_ask->price;
    }
    if (no_ask) {
        liquidity += no_ask->size * no_ask->price;
    }

    // Invert since higher liquidity = better
    return 1.0 - score_to_regime_score(liquidity,
        config_.favorable_liquidity,
        config_.neutral_liquidity,
        config_.unfavorable_liquidity);
}

double RegimeFilter::calculate_spread_score(const BinaryMarketBook& book) const {
    auto yes_bid = book.yes_book().best_bid();
    auto yes_ask = book.yes_book().best_ask();

    if (!yes_bid || !yes_ask || yes_ask->price <= 0) {
        return 0.5;  // Neutral if can't calculate
    }

    double spread_bps = (yes_ask->price - yes_bid->price) / yes_ask->price * 10000;
    return score_to_regime_score(spread_bps,
        config_.favorable_spread,
        config_.neutral_spread,
        config_.unfavorable_spread);
}

double RegimeFilter::calculate_time_score(const BinaryMarketBook& book) const {
    auto time_diff = book.end_date() - wall_now();
    double hours = std::chrono::duration_cast<std::chrono::minutes>(time_diff).count() / 60.0;

    if (hours <= 0) {
        return 0.0;  // Expired
    }

    // Invert since more time = better
    return 1.0 - score_to_regime_score(hours,
        config_.favorable_hours,
        config_.neutral_hours,
        config_.unfavorable_hours);
}

double RegimeFilter::calculate_momentum_score() const {
    double momentum = std::abs(calculate_recent_momentum());
    return score_to_regime_score(momentum,
        config_.favorable_momentum,
        (config_.favorable_momentum + config_.unfavorable_momentum) / 2,
        config_.unfavorable_momentum);
}

double RegimeFilter::score_to_regime_score(
    double raw,
    double favorable,
    double neutral,
    double unfavorable
) const {
    // Convert raw metric to 0-1 score where 1 = favorable
    // Lower raw values are better

    if (raw <= favorable) {
        return 1.0;
    } else if (raw <= neutral) {
        // Linear interpolation between favorable and neutral
        return 0.5 + 0.5 * (neutral - raw) / (neutral - favorable);
    } else if (raw <= unfavorable) {
        // Linear interpolation between neutral and unfavorable
        return 0.5 * (unfavorable - raw) / (unfavorable - neutral);
    } else {
        return 0.0;
    }
}

MarketRegime RegimeFilter::score_to_regime(double overall_score) const {
    if (overall_score >= 0.7) {
        return MarketRegime::FAVORABLE;
    } else if (overall_score >= 0.4) {
        return MarketRegime::NEUTRAL;
    } else if (overall_score >= 0.2) {
        return MarketRegime::UNFAVORABLE;
    } else {
        return MarketRegime::DANGEROUS;
    }
}

double RegimeFilter::calculate_hourly_volatility() const {
    std::lock_guard<std::mutex> lock(price_mutex_);

    if (price_history_.size() < 10) {
        return 0.01;  // Default 1% if not enough data
    }

    // Get prices from last hour
    auto cutoff = now() - std::chrono::hours(1);
    std::vector<double> prices;

    for (const auto& pp : price_history_) {
        if (pp.time >= cutoff) {
            prices.push_back(pp.price);
        }
    }

    if (prices.size() < 2) {
        return 0.01;
    }

    // Calculate log returns
    std::vector<double> returns;
    for (size_t i = 1; i < prices.size(); ++i) {
        returns.push_back(std::log(prices[i] / prices[i-1]));
    }

    // Standard deviation
    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double sq_sum = 0.0;
    for (double r : returns) {
        sq_sum += (r - mean) * (r - mean);
    }

    return std::sqrt(sq_sum / returns.size());
}

double RegimeFilter::calculate_recent_momentum() const {
    std::lock_guard<std::mutex> lock(price_mutex_);

    if (price_history_.size() < 2) {
        return 0.0;
    }

    // Get prices from last 15 minutes
    auto cutoff = now() - std::chrono::minutes(15);
    double first_price = 0.0;
    double last_price = 0.0;

    for (const auto& pp : price_history_) {
        if (pp.time >= cutoff) {
            if (first_price == 0.0) {
                first_price = pp.price;
            }
            last_price = pp.price;
        }
    }

    if (first_price <= 0 || last_price <= 0) {
        return 0.0;
    }

    return (last_price - first_price) / first_price;
}

} // namespace arb
