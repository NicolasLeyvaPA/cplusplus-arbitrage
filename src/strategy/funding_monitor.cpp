#include "strategy/funding_monitor.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <numeric>

namespace arb {

FundingMonitor::FundingMonitor(FuturesInterface& futures, const StrategyConfig& config)
    : futures_(futures), config_(config) {}

FundingMonitor::~FundingMonitor() { stop(); }

void FundingMonitor::start() {
    if (running_) return;
    running_ = true;

    // Initial fetch
    fetch_funding_rates();

    // Load history for each symbol
    for (const auto& symbol : config_.symbols) {
        try {
            auto history = futures_.get_funding_history(symbol, config_.funding_rate_lookback_periods);
            std::lock_guard<std::mutex> lock(mutex_);
            auto& hist = funding_history_[symbol];
            for (const auto& h : history) {
                hist.push_back(h.current_rate);
            }
            // Initialize EMA with the average of history
            if (!hist.empty()) {
                double sum = std::accumulate(hist.begin(), hist.end(), 0.0);
                ema_predictions_[symbol] = sum / hist.size();
            }
            spdlog::info("Loaded {} historical funding rates for {}", hist.size(), symbol);
        } catch (const std::exception& e) {
            spdlog::warn("Failed to load funding history for {}: {}", symbol, e.what());
        }
    }

    poll_thread_ = std::thread(&FundingMonitor::poll_loop, this);
    spdlog::info("FundingMonitor started");
}

void FundingMonitor::stop() {
    running_ = false;
    if (poll_thread_.joinable()) poll_thread_.join();
}

void FundingMonitor::poll_loop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        if (!running_) break;
        fetch_funding_rates();
    }
}

void FundingMonitor::fetch_funding_rates() {
    for (const auto& symbol : config_.symbols) {
        try {
            auto info = futures_.get_funding_info(symbol);
            std::lock_guard<std::mutex> lock(mutex_);
            current_funding_[symbol] = info;

            auto& hist = funding_history_[symbol];
            hist.push_back(info.current_rate);
            if (static_cast<int>(hist.size()) > config_.funding_rate_lookback_periods * 2) {
                hist.erase(hist.begin());
            }

            // Update EMA prediction
            double alpha = 0.3;
            auto ema_it = ema_predictions_.find(symbol);
            if (ema_it != ema_predictions_.end()) {
                ema_it->second = alpha * info.current_rate + (1.0 - alpha) * ema_it->second;
            } else {
                ema_predictions_[symbol] = info.current_rate;
            }

            spdlog::debug("{} funding rate: {:.6f} ({:.2f}% APY), EMA: {:.6f}",
                         symbol, info.current_rate, info.annualized_rate(),
                         ema_predictions_[symbol]);
        } catch (const std::exception& e) {
            spdlog::warn("Failed to fetch funding for {}: {}", symbol, e.what());
        }
    }
}

void FundingMonitor::refresh() {
    fetch_funding_rates();
}

FundingInfo FundingMonitor::get_funding(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = current_funding_.find(symbol);
    if (it != current_funding_.end()) return it->second;
    return FundingInfo{};
}

std::map<std::string, FundingInfo> FundingMonitor::get_all_funding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_funding_;
}

double FundingMonitor::average_funding_rate(const std::string& symbol, int periods) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = funding_history_.find(symbol);
    if (it == funding_history_.end() || it->second.empty()) return 0;

    const auto& hist = it->second;
    int n = std::min(periods, static_cast<int>(hist.size()));
    double sum = std::accumulate(hist.end() - n, hist.end(), 0.0);
    return sum / n;
}

double FundingMonitor::predicted_funding_rate(const std::string& symbol, double alpha) const {
    std::lock_guard<std::mutex> lock(mutex_);
    (void)alpha;  // alpha is applied in fetch_funding_rates
    auto it = ema_predictions_.find(symbol);
    if (it != ema_predictions_.end()) return it->second;
    return 0;
}

bool FundingMonitor::is_elevated_rate(const std::string& symbol, double multiplier) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto current_it = current_funding_.find(symbol);
    if (current_it == current_funding_.end()) return false;

    auto hist_it = funding_history_.find(symbol);
    if (hist_it == funding_history_.end() || hist_it->second.empty()) return false;

    const auto& hist = hist_it->second;
    int n = std::min(config_.funding_rate_lookback_periods, static_cast<int>(hist.size()));
    double avg = std::accumulate(hist.end() - n, hist.end(), 0.0) / n;

    return current_it->second.current_rate > avg * multiplier;
}

double FundingMonitor::compute_score(const std::string& symbol) const {
    // Must hold mutex
    auto it = current_funding_.find(symbol);
    if (it == current_funding_.end()) return 0;

    const auto& info = it->second;
    double score = 0;

    // Current rate component (40% weight)
    if (info.current_rate >= config_.min_funding_rate) {
        double rate_score = std::min(1.0, info.current_rate / (config_.min_funding_rate * 5));
        score += rate_score * 0.4;
    }

    // Average rate component (30% weight)
    double avg = 0;
    auto hist_it = funding_history_.find(symbol);
    if (hist_it != funding_history_.end() && !hist_it->second.empty()) {
        const auto& hist = hist_it->second;
        int n = std::min(config_.funding_rate_lookback_periods, static_cast<int>(hist.size()));
        avg = std::accumulate(hist.end() - n, hist.end(), 0.0) / n;
    }
    if (avg >= config_.min_avg_funding_rate) {
        double avg_score = std::min(1.0, avg / (config_.min_avg_funding_rate * 5));
        score += avg_score * 0.3;
    }

    // Predicted rate component (30% weight) - using EMA
    auto ema_it = ema_predictions_.find(symbol);
    double predicted = (ema_it != ema_predictions_.end()) ? ema_it->second : info.predicted_rate;
    if (predicted > 0) {
        score += std::min(1.0, predicted / config_.min_funding_rate) * 0.3;
    }

    return score;
}

std::vector<FundingMonitor::Opportunity> FundingMonitor::get_ranked_opportunities() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Opportunity> opportunities;
    for (const auto& [symbol, info] : current_funding_) {
        Opportunity opp;
        opp.symbol = symbol;
        opp.current_rate = info.current_rate;
        opp.avg_rate = 0;

        auto hist_it = funding_history_.find(symbol);
        if (hist_it != funding_history_.end() && !hist_it->second.empty()) {
            const auto& hist = hist_it->second;
            int n = std::min(config_.funding_rate_lookback_periods, static_cast<int>(hist.size()));
            opp.avg_rate = std::accumulate(hist.end() - n, hist.end(), 0.0) / n;
        }

        opp.annualized_yield = info.annualized_rate();
        opp.score = compute_score(symbol);
        opportunities.push_back(opp);
    }

    std::sort(opportunities.begin(), opportunities.end(),
              [](const Opportunity& a, const Opportunity& b) { return a.score > b.score; });

    return opportunities;
}

} // namespace arb
