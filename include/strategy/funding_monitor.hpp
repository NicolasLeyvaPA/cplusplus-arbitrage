#pragma once

#include "common/types.hpp"
#include "config/config.hpp"
#include "exchange/binance_futures.hpp"
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

namespace arb {

/**
 * Monitors funding rates across configured symbols.
 * Polls Binance futures API and maintains history for scoring.
 */
class FundingMonitor {
public:
    FundingMonitor(BinanceFutures& futures, const StrategyConfig& config);
    ~FundingMonitor();

    void start();
    void stop();
    bool is_running() const { return running_.load(); }

    FundingInfo get_funding(const std::string& symbol) const;
    std::map<std::string, FundingInfo> get_all_funding() const;

    struct Opportunity {
        std::string symbol;
        double current_rate{0};
        double avg_rate{0};
        double annualized_yield{0};
        double score{0};
    };

    std::vector<Opportunity> get_ranked_opportunities() const;
    double average_funding_rate(const std::string& symbol, int periods) const;

    // Manual refresh (bypasses polling interval)
    void refresh();

private:
    void poll_loop();
    void fetch_funding_rates();
    double compute_score(const std::string& symbol) const;

    BinanceFutures& futures_;
    StrategyConfig config_;

    mutable std::mutex mutex_;
    std::map<std::string, FundingInfo> current_funding_;
    std::map<std::string, std::vector<double>> funding_history_;

    std::atomic<bool> running_{false};
    std::thread poll_thread_;
};

} // namespace arb
