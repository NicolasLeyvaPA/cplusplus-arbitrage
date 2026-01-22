#include "strategy/volatility_fair_value.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <numeric>
#include <regex>

namespace arb {

VolatilityFairValueStrategy::VolatilityFairValueStrategy(
    const StrategyConfig& base_config,
    const Config& config
)
    : StrategyBase("S5_VolatilityFairValue", base_config)
    , vol_config_(config)
{
    spdlog::info("VolatilityFairValueStrategy initialized: min_prob_edge={:.1f}%, min_cents_edge={:.2f}c",
                 vol_config_.min_probability_edge * 100,
                 vol_config_.min_cents_edge);
}

std::vector<Signal> VolatilityFairValueStrategy::evaluate(
    const BinaryMarketBook& book,
    const BtcPrice& btc_price,
    Timestamp now
) {
    std::vector<Signal> signals;

    if (!enabled_) return signals;

    // Update volatility estimate
    update_btc_price(btc_price.mid, now);

    // Check market is tradeable
    if (!is_market_tradeable(book)) {
        return signals;
    }

    // Parse market parameters
    double strike = parse_strike_from_market(book.market_id(), book.question());
    if (strike <= 0) {
        spdlog::debug("Could not parse strike from market: {}", book.question());
        return signals;
    }

    double ttx = parse_time_to_expiry(book.end_date());
    if (ttx <= 0) {
        spdlog::debug("Market expired or invalid TTX: {}", book.market_id());
        return signals;
    }

    // Calculate fair value
    auto fv = calculate_fair_value(book, btc_price.mid, strike, ttx);

    if (!fv.is_tradeable) {
        spdlog::debug("Not tradeable: {}", fv.reason);
        return signals;
    }

    // Calculate edge in cents
    double yes_ask = book.yes_book().best_ask() ? book.yes_book().best_ask()->price : 1.0;
    double no_ask = book.no_book().best_ask() ? book.no_book().best_ask()->price : 1.0;

    double cents_edge = std::abs(fv.probability_edge) * 100;  // Convert to cents (since prices are 0-1)

    // Check minimum thresholds
    if (std::abs(fv.probability_edge) < vol_config_.min_probability_edge) {
        spdlog::debug("Edge too small: {:.2f}% < {:.2f}%",
                     std::abs(fv.probability_edge) * 100,
                     vol_config_.min_probability_edge * 100);
        return signals;
    }

    if (cents_edge < vol_config_.min_cents_edge) {
        spdlog::debug("Cents edge too small: {:.2f}c < {:.2f}c",
                     cents_edge, vol_config_.min_cents_edge);
        return signals;
    }

    // Determine direction
    bool should_buy_yes = fv.fair_probability > fv.market_probability;

    // Calculate Kelly-optimal size
    double edge = std::abs(fv.probability_edge);
    double prob = should_buy_yes ? fv.fair_probability : (1.0 - fv.fair_probability);
    double max_size = config_.max_notional_per_trade / (should_buy_yes ? yes_ask : no_ask);
    double size = kelly_size(edge, prob, max_size);

    if (size < 1.0) {  // Minimum $1 trade
        return signals;
    }

    // Generate signal
    Signal signal;
    signal.strategy_name = name_;
    signal.market_id = book.market_id();
    signal.token_id = should_buy_yes ? book.yes_token_id() : book.no_token_id();
    signal.side = Side::BUY;
    signal.target_price = should_buy_yes ? yes_ask : no_ask;
    signal.target_size = size;
    signal.expected_edge = edge;
    signal.confidence = std::min(1.0, edge / vol_config_.min_probability_edge);
    signal.generated_at = now;
    signal.reason = fmt::format(
        "Vol-adj fair value: fair_prob={:.1f}%, market_prob={:.1f}%, edge={:.1f}%, "
        "realized_vol={:.1f}%, implied_vol={:.1f}%",
        fv.fair_probability * 100,
        fv.market_probability * 100,
        fv.probability_edge * 100,
        fv.realized_vol * 100,
        fv.implied_vol * 100
    );

    signals.push_back(signal);
    signals_generated_++;

    spdlog::info("Signal generated: {} {} @ {:.4f} x {:.2f}, edge={:.2f}%",
                should_buy_yes ? "BUY YES" : "BUY NO",
                signal.market_id,
                signal.target_price,
                signal.target_size,
                edge * 100);

    return signals;
}

void VolatilityFairValueStrategy::update_btc_price(double price, Timestamp time) {
    // Add to history
    price_history_.push_back({price, time});

    // Maintain window size
    size_t max_samples = vol_config_.vol_lookback_periods * 3600 / vol_config_.vol_sample_interval_seconds;
    while (price_history_.size() > max_samples) {
        price_history_.pop_front();
    }

    // Invalidate cache
    cached_vol_ = 0.0;
}

double VolatilityFairValueStrategy::current_volatility() const {
    if (price_history_.size() < 10) {
        return vol_config_.default_annualized_vol;
    }

    // Use cached value if recent
    auto now_time = now();
    if (cached_vol_ > 0 && (now_time - vol_cache_time_) < std::chrono::minutes(5)) {
        return cached_vol_;
    }

    cached_vol_ = calculate_realized_vol();
    vol_cache_time_ = now_time;
    return cached_vol_;
}

double VolatilityFairValueStrategy::annualized_volatility() const {
    return current_volatility();  // Already annualized in calculate_realized_vol
}

VolatilityFairValueStrategy::FairValueResult VolatilityFairValueStrategy::calculate_fair_value(
    const BinaryMarketBook& book,
    double btc_current,
    double btc_strike,
    double time_to_expiry_years
) const {
    FairValueResult result;

    // Get realized volatility
    result.realized_vol = current_volatility();

    // Calculate fair probability using Black-Scholes
    double r = 0.0;  // Risk-free rate (assume 0 for simplicity)
    result.fair_probability = bs_probability(btc_current, btc_strike, time_to_expiry_years, r, result.realized_vol);

    // Extract market probability
    result.market_probability = extract_market_probability(book);

    // Calculate implied volatility from market price
    result.implied_vol = implied_volatility_from_price(result.market_probability, btc_current, btc_strike, time_to_expiry_years);

    // Calculate edge
    result.probability_edge = result.fair_probability - result.market_probability;

    // Check if tradeable
    if (result.fair_probability > vol_config_.max_probability || result.fair_probability < vol_config_.min_probability) {
        result.is_tradeable = false;
        result.reason = fmt::format("Fair probability {:.1f}% outside bounds", result.fair_probability * 100);
        return result;
    }

    if (result.market_probability > vol_config_.max_probability || result.market_probability < vol_config_.min_probability) {
        result.is_tradeable = false;
        result.reason = fmt::format("Market probability {:.1f}% outside bounds", result.market_probability * 100);
        return result;
    }

    result.is_tradeable = true;
    return result;
}

double VolatilityFairValueStrategy::kelly_size(double edge, double probability, double max_size) const {
    // Kelly criterion: f* = (p*b - q) / b where b = 1 (even money)
    // For binary outcomes: f* = 2*p - 1 (when edge = p - 0.5)
    // Adjusted for actual edge: f* = edge / (1 - probability)

    double q = 1.0 - probability;
    if (q <= 0.01) q = 0.01;  // Avoid division by very small numbers

    double kelly_full = edge / q;

    // Apply Kelly fraction (reduce sizing for safety)
    double kelly_adjusted = kelly_full * vol_config_.kelly_fraction;

    // Cap at max size
    double size = std::min(kelly_adjusted * max_size, max_size);

    // Ensure minimum size
    if (size < 1.0) size = 0.0;

    return size;
}

double VolatilityFairValueStrategy::calculate_realized_vol() const {
    if (price_history_.size() < 10) {
        return vol_config_.default_annualized_vol;
    }

    // Calculate log returns
    std::vector<double> returns;
    for (size_t i = 1; i < price_history_.size(); ++i) {
        double ret = std::log(price_history_[i].price / price_history_[i-1].price);
        returns.push_back(ret);
    }

    // Calculate standard deviation
    double vol = calculate_log_returns_vol(returns);

    // Annualize (assume samples are vol_sample_interval_seconds apart)
    double samples_per_year = 365.25 * 24 * 3600 / vol_config_.vol_sample_interval_seconds;
    double annualized = vol * std::sqrt(samples_per_year);

    return annualized;
}

double VolatilityFairValueStrategy::calculate_log_returns_vol(const std::vector<double>& returns) const {
    if (returns.empty()) return 0.0;

    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();

    double sq_sum = 0.0;
    for (double r : returns) {
        sq_sum += (r - mean) * (r - mean);
    }

    return std::sqrt(sq_sum / returns.size());
}

double VolatilityFairValueStrategy::normal_cdf(double x) const {
    // Approximation using error function
    return 0.5 * (1.0 + std::erf(x / std::sqrt(2.0)));
}

double VolatilityFairValueStrategy::normal_pdf(double x) const {
    return std::exp(-0.5 * x * x) / std::sqrt(2.0 * M_PI);
}

double VolatilityFairValueStrategy::bs_d1(double S, double K, double T, double r, double sigma) const {
    if (T <= 0 || sigma <= 0) return 0.0;
    return (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
}

double VolatilityFairValueStrategy::bs_d2(double d1, double sigma, double T) const {
    return d1 - sigma * std::sqrt(T);
}

double VolatilityFairValueStrategy::bs_probability(double S, double K, double T, double r, double sigma) const {
    // Probability that S > K at time T (digital call probability)
    // This is N(d2) in Black-Scholes framework
    if (T <= 0) {
        return S >= K ? 1.0 : 0.0;
    }

    double d1 = bs_d1(S, K, T, r, sigma);
    double d2 = bs_d2(d1, sigma, T);

    return normal_cdf(d2);
}

double VolatilityFairValueStrategy::extract_market_probability(const BinaryMarketBook& book) const {
    // Market probability = 1 - YES_ask (if we buy YES at ask, we're paying for that probability)
    // Or equivalently: NO_ask (the probability of NO happening)

    auto yes_ask = book.yes_book().best_ask();
    auto no_ask = book.no_book().best_ask();

    if (yes_ask && no_ask) {
        // Midpoint of implied probabilities
        double prob_from_yes = 1.0 - yes_ask->price;  // What we pay for YES is the complement
        double prob_from_no = no_ask->price;  // NO price is market's P(price < strike)

        // Actually for a "BTC above X" market:
        // YES price ≈ P(BTC > X)
        // NO price ≈ P(BTC < X) = 1 - P(BTC > X)
        // So market P(BTC > X) ≈ YES_mid or (YES_bid + YES_ask) / 2

        auto yes_bid = book.yes_book().best_bid();
        double yes_mid = yes_ask->price;
        if (yes_bid) {
            yes_mid = (yes_bid->price + yes_ask->price) / 2.0;
        }

        return yes_mid;  // YES price IS the probability of BTC > strike
    }

    return 0.5;  // Default
}

double VolatilityFairValueStrategy::implied_volatility_from_price(
    double market_prob,
    double S,
    double K,
    double T
) const {
    // Newton-Raphson to find vol that matches market probability
    double sigma = vol_config_.default_annualized_vol;
    double r = 0.0;

    for (int i = 0; i < 20; ++i) {
        double calc_prob = bs_probability(S, K, T, r, sigma);
        double error = calc_prob - market_prob;

        if (std::abs(error) < 0.001) break;

        // Vega-like derivative (sensitivity to vol)
        double d1 = bs_d1(S, K, T, r, sigma);
        double vega = normal_pdf(d1) * std::sqrt(T);

        if (vega < 0.001) break;

        sigma -= error / vega;
        sigma = std::max(0.01, std::min(3.0, sigma));  // Bound vol between 1% and 300%
    }

    return sigma;
}

double VolatilityFairValueStrategy::parse_strike_from_market(
    const std::string& market_id,
    const std::string& question
) const {
    // Try to extract strike price from market question
    // Examples:
    // "Will BTC be above $100,000 on Jan 31?"
    // "BTC 15m: Above $98,500?"

    std::regex price_regex(R"(\$([0-9,]+(?:\.[0-9]+)?))");
    std::smatch match;

    if (std::regex_search(question, match, price_regex)) {
        std::string price_str = match[1].str();
        // Remove commas
        price_str.erase(std::remove(price_str.begin(), price_str.end(), ','), price_str.end());
        try {
            return std::stod(price_str);
        } catch (...) {
            return 0.0;
        }
    }

    return 0.0;
}

double VolatilityFairValueStrategy::parse_time_to_expiry(const WallClock& end_date) const {
    auto now_time = wall_now();
    auto diff = end_date - now_time;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(diff).count();

    if (seconds <= 0) return 0.0;

    // Convert to years
    return static_cast<double>(seconds) / (365.25 * 24 * 3600);
}

bool VolatilityFairValueStrategy::is_market_tradeable(const BinaryMarketBook& book) const {
    if (!book.has_liquidity()) {
        return false;
    }

    auto yes_ask = book.yes_book().best_ask();
    auto yes_bid = book.yes_book().best_bid();
    auto no_ask = book.no_book().best_ask();

    if (!yes_ask || !no_ask) {
        return false;
    }

    // Check spread
    if (yes_bid) {
        double spread = (yes_ask->price - yes_bid->price) / yes_ask->price;
        if (spread > vol_config_.max_spread_percent) {
            return false;
        }
    }

    // Check liquidity
    if (yes_ask->size * yes_ask->price < vol_config_.min_liquidity_usd) {
        return false;
    }

    return true;
}

} // namespace arb
