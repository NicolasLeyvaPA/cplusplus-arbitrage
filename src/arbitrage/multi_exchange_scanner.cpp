#include "arbitrage/multi_exchange_scanner.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace arb {

MultiExchangeScanner::MultiExchangeScanner(const Config& config)
    : config_(config)
{
    // Initialize default exchange fees (taker fees in bps)
    // These should be updated with actual account tier fees
    exchange_fees_ = {
        {"binance",     {10, 10}},   // 0.10% maker/taker
        {"binance_us",  {10, 10}},
        {"kraken",      {16, 26}},   // 0.16% maker, 0.26% taker
        {"coinbase",    {40, 60}},   // 0.40% maker, 0.60% taker
        {"okx",         {8, 10}},    // 0.08% maker, 0.10% taker
        {"bybit",       {10, 10}},
        {"kucoin",      {10, 10}},
        {"gateio",      {20, 20}},
        {"htx",         {20, 20}},   // Formerly Huobi
        {"mexc",        {0, 5}},     // 0% maker, 0.05% taker
        {"bitget",      {10, 10}},
        {"bithumb",     {25, 25}},   // Korean exchange
        {"upbit",       {25, 25}},   // Korean exchange
    };

    spdlog::info("MultiExchangeScanner initialized with {} exchanges, min_spread={}bps",
                 exchange_fees_.size(), config_.min_net_spread_bps);
}

void MultiExchangeScanner::set_exchange_fees(const std::string& exchange,
                                              double maker_bps, double taker_bps) {
    std::lock_guard<std::mutex> lock(mutex_);
    exchange_fees_[exchange] = {maker_bps, taker_bps};
}

double MultiExchangeScanner::get_taker_fee_bps(const std::string& exchange) const {
    auto it = exchange_fees_.find(exchange);
    if (it != exchange_fees_.end()) {
        return it->second.second;
    }
    return 20.0;  // Conservative default: 0.20%
}

double MultiExchangeScanner::get_maker_fee_bps(const std::string& exchange) const {
    auto it = exchange_fees_.find(exchange);
    if (it != exchange_fees_.end()) {
        return it->second.first;
    }
    return 20.0;
}

void MultiExchangeScanner::update_quote(const ExchangeQuote& quote) {
    std::lock_guard<std::mutex> lock(mutex_);

    quotes_[quote.exchange][quote.symbol] = quote;
    stats_.quotes_processed++;
}

std::vector<CrossExchangeOpportunity> MultiExchangeScanner::scan_cross_exchange() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CrossExchangeOpportunity> opportunities;

    stats_.scan_count++;

    // For each symbol, compare across all exchange pairs
    std::map<std::string, std::vector<ExchangeQuote>> by_symbol;

    // Group quotes by symbol
    for (const auto& [exchange, symbols] : quotes_) {
        for (const auto& [symbol, quote] : symbols) {
            if (quote.is_valid && !quote.is_stale(config_.max_quote_age_us)) {
                by_symbol[symbol].push_back(quote);
            }
        }
    }

    // For each symbol with quotes from multiple exchanges
    for (const auto& [symbol, quotes] : by_symbol) {
        if (quotes.size() < 2) continue;

        // Compare all pairs of exchanges
        for (size_t i = 0; i < quotes.size(); i++) {
            for (size_t j = 0; j < quotes.size(); j++) {
                if (i == j) continue;

                // Buy on exchange i (use ask), sell on exchange j (use bid)
                auto opp = evaluate_pair(quotes[i], quotes[j]);

                if (opp.net_spread_bps > 0) {
                    stats_.opportunities_detected++;
                    stats_.best_spread_seen_bps = std::max(
                        stats_.best_spread_seen_bps, opp.net_spread_bps
                    );

                    if (opp.is_actionable) {
                        stats_.actionable_opportunities++;
                        stats_.total_theoretical_profit += opp.expected_profit_usd;

                        if (on_opportunity_) {
                            on_opportunity_(opp);
                        }
                    }

                    opportunities.push_back(opp);
                }
            }
        }
    }

    // Sort by net spread (best first)
    std::sort(opportunities.begin(), opportunities.end(),
              [](const auto& a, const auto& b) {
                  return a.net_spread_bps > b.net_spread_bps;
              });

    return opportunities;
}

CrossExchangeOpportunity MultiExchangeScanner::evaluate_pair(
    const ExchangeQuote& buy_quote,
    const ExchangeQuote& sell_quote
) {
    CrossExchangeOpportunity opp;

    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    opp.buy_exchange = buy_quote.exchange;
    opp.sell_exchange = sell_quote.exchange;
    opp.symbol = buy_quote.symbol;

    opp.buy_price = buy_quote.ask_price;   // We pay the ask to buy
    opp.sell_price = sell_quote.bid_price; // We receive the bid to sell

    // Calculate spreads
    opp.gross_spread_bps = (opp.sell_price - opp.buy_price) / opp.buy_price * 10000;

    double buy_fee = get_taker_fee_bps(buy_quote.exchange);
    double sell_fee = get_taker_fee_bps(sell_quote.exchange);
    opp.net_spread_bps = opp.gross_spread_bps - buy_fee - sell_fee;

    // Size constraints
    opp.buy_size_available = buy_quote.ask_size;
    opp.sell_size_available = sell_quote.bid_size;
    opp.max_executable_size = std::min(buy_quote.ask_size, sell_quote.bid_size);

    // Limit by config
    double max_size_units = config_.max_size_usd / opp.buy_price;
    opp.max_executable_size = std::min(opp.max_executable_size, max_size_units);

    // Calculate expected profit
    opp.profit_per_unit = (opp.sell_price - opp.buy_price) -
                          (opp.buy_price * buy_fee / 10000) -
                          (opp.sell_price * sell_fee / 10000);
    opp.expected_profit_usd = opp.profit_per_unit * opp.max_executable_size;

    // Timing
    opp.detected_at_us = now_us;
    opp.buy_quote_age_us = buy_quote.age_us();
    opp.sell_quote_age_us = sell_quote.age_us();

    // Calculate confidence based on quote freshness
    double freshness_score = 1.0 - std::max(opp.buy_quote_age_us, opp.sell_quote_age_us) /
                                   (double)config_.max_quote_age_us;
    freshness_score = std::max(0.0, freshness_score);

    // Size confidence
    double size_score = std::min(1.0, opp.max_executable_size * opp.buy_price / config_.max_size_usd);

    opp.confidence = freshness_score * size_score;

    // Determine if actionable
    opp.is_actionable = true;
    std::vector<std::string> reasons;

    if (opp.net_spread_bps < config_.min_net_spread_bps) {
        opp.is_actionable = false;
        reasons.push_back(fmt::format("spread {:.2f}bps < {:.2f}bps min",
                                      opp.net_spread_bps, config_.min_net_spread_bps));
    }

    if (opp.expected_profit_usd < config_.min_profit_usd) {
        opp.is_actionable = false;
        reasons.push_back(fmt::format("profit ${:.2f} < ${:.2f} min",
                                      opp.expected_profit_usd, config_.min_profit_usd));
    }

    double position_value = opp.max_executable_size * opp.buy_price;
    if (position_value < config_.min_size_usd) {
        opp.is_actionable = false;
        reasons.push_back(fmt::format("size ${:.0f} < ${:.0f} min",
                                      position_value, config_.min_size_usd));
    }

    if (config_.require_both_fresh) {
        if (buy_quote.is_stale(config_.max_quote_age_us) ||
            sell_quote.is_stale(config_.max_quote_age_us)) {
            opp.is_actionable = false;
            reasons.push_back("stale quotes");
        }
    }

    if (opp.is_actionable) {
        opp.reason = fmt::format("NET +{:.2f}bps, ${:.2f} profit on {:.4f} units",
                                 opp.net_spread_bps, opp.expected_profit_usd,
                                 opp.max_executable_size);
    } else {
        opp.reason = fmt::format("{}", fmt::join(reasons, "; "));
    }

    return opp;
}

std::vector<TriangularOpportunity> MultiExchangeScanner::scan_triangular(
    const std::string& exchange
) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TriangularOpportunity> opportunities;

    auto it = quotes_.find(exchange);
    if (it == quotes_.end()) return opportunities;

    const auto& symbols = it->second;

    // Find common triangles: BTC/USDT, ETH/USDT, BTC/ETH
    // or: BTC/USDT, ETH/USDT, ETH/BTC

    // For simplicity, look for standard crypto triangles
    std::vector<std::tuple<std::string, std::string, std::string>> triangles = {
        {"BTC/USDT", "ETH/USDT", "ETH/BTC"},
        {"BTC/USDT", "ETH/USDT", "BTC/ETH"},
        {"BTC/USDT", "BNB/USDT", "BNB/BTC"},
        {"ETH/USDT", "BNB/USDT", "BNB/ETH"},
    };

    for (const auto& [p1, p2, p3] : triangles) {
        auto q1_it = symbols.find(p1);
        auto q2_it = symbols.find(p2);
        auto q3_it = symbols.find(p3);

        if (q1_it == symbols.end() || q2_it == symbols.end() || q3_it == symbols.end()) {
            continue;
        }

        if (!q1_it->second.is_valid || !q2_it->second.is_valid || !q3_it->second.is_valid) {
            continue;
        }

        auto opp = evaluate_triangle(exchange, q1_it->second, q2_it->second, q3_it->second);

        if (opp.gross_profit_bps > 0) {
            if (on_triangular_ && opp.is_actionable) {
                on_triangular_(opp);
            }
            opportunities.push_back(opp);
        }
    }

    return opportunities;
}

TriangularOpportunity MultiExchangeScanner::evaluate_triangle(
    const std::string& exchange,
    const ExchangeQuote& q1,
    const ExchangeQuote& q2,
    const ExchangeQuote& q3
) {
    TriangularOpportunity opp;

    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    opp.exchange = exchange;
    opp.pair_1 = q1.symbol;
    opp.pair_2 = q2.symbol;
    opp.pair_3 = q3.symbol;
    opp.detected_at_us = now_us;

    // Calculate cycle:
    // Assuming: BTC/USDT, ETH/USDT, ETH/BTC
    // 1 BTC → sell at bid → USDT
    // USDT → buy ETH at ask → ETH
    // ETH → sell at bid for BTC → BTC

    opp.rate_1 = q1.bid_price;  // Sell BTC for USDT
    opp.rate_2 = 1.0 / q2.ask_price;  // Buy ETH with USDT (invert because ETH/USDT)
    opp.rate_3 = q3.bid_price;  // Sell ETH for BTC (or need to check pair direction)

    // This is simplified - real implementation needs to handle pair directions
    opp.cycle_return = opp.rate_1 * opp.rate_2 * opp.rate_3;

    opp.gross_profit_bps = (opp.cycle_return - 1.0) * 10000;

    // Subtract 3 taker fees
    double fee = get_taker_fee_bps(exchange);
    opp.net_profit_bps = opp.gross_profit_bps - (3 * fee);

    // Size is limited by smallest leg
    opp.max_size = std::min({q1.bid_size, q2.ask_size * q2.ask_price / q1.bid_price, q3.bid_size});
    opp.expected_profit_usd = (opp.net_profit_bps / 10000) * opp.max_size * q1.bid_price;

    opp.is_actionable = (opp.net_profit_bps >= config_.min_net_spread_bps &&
                         opp.expected_profit_usd >= config_.min_profit_usd);

    if (opp.is_actionable) {
        opp.reason = fmt::format("Triangular +{:.2f}bps net, ${:.2f} profit",
                                 opp.net_profit_bps, opp.expected_profit_usd);
    } else {
        opp.reason = fmt::format("gross={:.2f}bps, net={:.2f}bps (3x{:.0f}bps fees)",
                                 opp.gross_profit_bps, opp.net_profit_bps, fee);
    }

    return opp;
}

std::vector<FundingRateOpportunity> MultiExchangeScanner::scan_funding_rates() {
    // This would require perpetual futures data
    // Placeholder for now
    return {};
}

}  // namespace arb
