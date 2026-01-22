#include "core/exposure_manager.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

namespace arb {

bool ExposureManager::SoftLimits::validate() const {
    if (max_total_exposure > HARD_MAX_TOTAL_EXPOSURE) return false;
    if (max_market_exposure > HARD_MAX_MARKET_EXPOSURE) return false;
    if (max_position_size > HARD_MAX_POSITION_SIZE) return false;
    if (max_open_positions > HARD_MAX_OPEN_POSITIONS) return false;
    if (max_positions_per_market > HARD_MAX_POSITIONS_PER_MARKET) return false;
    return true;
}

void ExposureManager::SoftLimits::clamp_to_hard_limits() {
    max_total_exposure = std::min(max_total_exposure, HARD_MAX_TOTAL_EXPOSURE);
    max_market_exposure = std::min(max_market_exposure, HARD_MAX_MARKET_EXPOSURE);
    max_position_size = std::min(max_position_size, HARD_MAX_POSITION_SIZE);
    max_open_positions = std::min(max_open_positions, HARD_MAX_OPEN_POSITIONS);
    max_positions_per_market = std::min(max_positions_per_market, HARD_MAX_POSITIONS_PER_MARKET);
}

ExposureManager::ExposureManager(const SoftLimits& limits)
    : soft_limits_(limits)
{
    // Always clamp to hard limits
    soft_limits_.clamp_to_hard_limits();

    spdlog::info("ExposureManager: total_limit=${:.0f}, market_limit=${:.0f}, "
                 "position_limit=${:.0f}, max_positions={}",
                 soft_limits_.max_total_exposure,
                 soft_limits_.max_market_exposure,
                 soft_limits_.max_position_size,
                 soft_limits_.max_open_positions);
}

ExposureCheckResult ExposureManager::can_open_position(
    const std::string& market_id,
    double notional
) const {
    ExposureCheckResult result;
    result.current_exposure = total_exposure_.load();

    std::lock_guard<std::mutex> lock(mutex_);

    double current_total = total_exposure_.load();
    int current_count = open_positions_.load();

    auto market_it = market_exposures_.find(market_id);
    double current_market = (market_it != market_exposures_.end()) ? market_it->second : 0.0;

    auto count_it = market_position_counts_.find(market_id);
    int market_count = (count_it != market_position_counts_.end()) ? count_it->second : 0;

    double new_total = current_total + notional;
    double new_market = current_market + notional;
    int new_count = current_count + 1;
    int new_market_count = market_count + 1;

    // Check hard limits first (cannot be bypassed)
    if (new_total > HARD_MAX_TOTAL_EXPOSURE) {
        result.rejection_reason = fmt::format(
            "HARD LIMIT: Total exposure ${:.2f} would exceed ${:.0f}",
            new_total, HARD_MAX_TOTAL_EXPOSURE);
        result.limit = HARD_MAX_TOTAL_EXPOSURE;
        result.headroom = std::max(0.0, HARD_MAX_TOTAL_EXPOSURE - current_total);
        return result;
    }

    if (new_market > HARD_MAX_MARKET_EXPOSURE) {
        result.rejection_reason = fmt::format(
            "HARD LIMIT: Market exposure ${:.2f} would exceed ${:.0f}",
            new_market, HARD_MAX_MARKET_EXPOSURE);
        result.limit = HARD_MAX_MARKET_EXPOSURE;
        result.headroom = std::max(0.0, HARD_MAX_MARKET_EXPOSURE - current_market);
        return result;
    }

    if (notional > HARD_MAX_POSITION_SIZE) {
        result.rejection_reason = fmt::format(
            "HARD LIMIT: Position size ${:.2f} exceeds ${:.0f}",
            notional, HARD_MAX_POSITION_SIZE);
        result.limit = HARD_MAX_POSITION_SIZE;
        result.headroom = HARD_MAX_POSITION_SIZE;
        return result;
    }

    if (new_count > HARD_MAX_OPEN_POSITIONS) {
        result.rejection_reason = fmt::format(
            "HARD LIMIT: {} positions would exceed max {}",
            new_count, HARD_MAX_OPEN_POSITIONS);
        result.limit = HARD_MAX_OPEN_POSITIONS;
        result.headroom = 0;
        return result;
    }

    if (new_market_count > HARD_MAX_POSITIONS_PER_MARKET) {
        result.rejection_reason = fmt::format(
            "HARD LIMIT: {} positions in market would exceed max {}",
            new_market_count, HARD_MAX_POSITIONS_PER_MARKET);
        result.limit = HARD_MAX_POSITIONS_PER_MARKET;
        result.headroom = 0;
        return result;
    }

    // Check soft limits
    if (new_total > soft_limits_.max_total_exposure) {
        result.rejection_reason = fmt::format(
            "Total exposure ${:.2f} would exceed limit ${:.0f}",
            new_total, soft_limits_.max_total_exposure);
        result.limit = soft_limits_.max_total_exposure;
        result.headroom = std::max(0.0, soft_limits_.max_total_exposure - current_total);
        return result;
    }

    if (new_market > soft_limits_.max_market_exposure) {
        result.rejection_reason = fmt::format(
            "Market exposure ${:.2f} would exceed limit ${:.0f}",
            new_market, soft_limits_.max_market_exposure);
        result.limit = soft_limits_.max_market_exposure;
        result.headroom = std::max(0.0, soft_limits_.max_market_exposure - current_market);
        return result;
    }

    if (notional > soft_limits_.max_position_size) {
        result.rejection_reason = fmt::format(
            "Position size ${:.2f} exceeds limit ${:.0f}",
            notional, soft_limits_.max_position_size);
        result.limit = soft_limits_.max_position_size;
        result.headroom = soft_limits_.max_position_size;
        return result;
    }

    if (new_count > soft_limits_.max_open_positions) {
        result.rejection_reason = fmt::format(
            "{} positions would exceed limit {}",
            new_count, soft_limits_.max_open_positions);
        result.limit = soft_limits_.max_open_positions;
        result.headroom = 0;
        return result;
    }

    if (new_market_count > soft_limits_.max_positions_per_market) {
        result.rejection_reason = fmt::format(
            "{} positions in market would exceed limit {}",
            new_market_count, soft_limits_.max_positions_per_market);
        result.limit = soft_limits_.max_positions_per_market;
        result.headroom = 0;
        return result;
    }

    // All checks passed
    result.allowed = true;
    result.current_exposure = current_total;
    result.limit = soft_limits_.max_total_exposure;
    result.headroom = soft_limits_.max_total_exposure - new_total;

    return result;
}

ExposureCheckResult ExposureManager::can_increase_position(
    const std::string& market_id,
    const std::string& token_id,
    double additional_notional
) const {
    ExposureCheckResult result;

    std::lock_guard<std::mutex> lock(mutex_);

    double current_total = total_exposure_.load();

    auto market_it = market_exposures_.find(market_id);
    double current_market = (market_it != market_exposures_.end()) ? market_it->second : 0.0;

    auto pos_it = position_exposures_.find(token_id);
    double current_position = (pos_it != position_exposures_.end()) ? pos_it->second : 0.0;

    double new_total = current_total + additional_notional;
    double new_market = current_market + additional_notional;
    double new_position = current_position + additional_notional;

    // Check hard limits
    if (new_total > HARD_MAX_TOTAL_EXPOSURE) {
        result.rejection_reason = fmt::format(
            "HARD LIMIT: Total exposure ${:.2f} would exceed ${:.0f}",
            new_total, HARD_MAX_TOTAL_EXPOSURE);
        result.headroom = std::max(0.0, HARD_MAX_TOTAL_EXPOSURE - current_total);
        return result;
    }

    if (new_market > HARD_MAX_MARKET_EXPOSURE) {
        result.rejection_reason = fmt::format(
            "HARD LIMIT: Market exposure ${:.2f} would exceed ${:.0f}",
            new_market, HARD_MAX_MARKET_EXPOSURE);
        result.headroom = std::max(0.0, HARD_MAX_MARKET_EXPOSURE - current_market);
        return result;
    }

    if (new_position > HARD_MAX_POSITION_SIZE) {
        result.rejection_reason = fmt::format(
            "HARD LIMIT: Position size ${:.2f} would exceed ${:.0f}",
            new_position, HARD_MAX_POSITION_SIZE);
        result.headroom = std::max(0.0, HARD_MAX_POSITION_SIZE - current_position);
        return result;
    }

    // Check soft limits
    if (new_total > soft_limits_.max_total_exposure) {
        result.rejection_reason = fmt::format(
            "Total exposure ${:.2f} would exceed limit ${:.0f}",
            new_total, soft_limits_.max_total_exposure);
        result.headroom = std::max(0.0, soft_limits_.max_total_exposure - current_total);
        return result;
    }

    if (new_market > soft_limits_.max_market_exposure) {
        result.rejection_reason = fmt::format(
            "Market exposure ${:.2f} would exceed limit ${:.0f}",
            new_market, soft_limits_.max_market_exposure);
        result.headroom = std::max(0.0, soft_limits_.max_market_exposure - current_market);
        return result;
    }

    if (new_position > soft_limits_.max_position_size) {
        result.rejection_reason = fmt::format(
            "Position size ${:.2f} would exceed limit ${:.0f}",
            new_position, soft_limits_.max_position_size);
        result.headroom = std::max(0.0, soft_limits_.max_position_size - current_position);
        return result;
    }

    result.allowed = true;
    result.current_exposure = current_total;
    result.headroom = std::min({
        soft_limits_.max_total_exposure - new_total,
        soft_limits_.max_market_exposure - new_market,
        soft_limits_.max_position_size - new_position
    });

    return result;
}

void ExposureManager::record_position_opened(
    const std::string& market_id,
    const std::string& token_id,
    double notional
) {
    std::lock_guard<std::mutex> lock(mutex_);

    market_exposures_[market_id] += notional;
    position_exposures_[token_id] = notional;
    market_position_counts_[market_id]++;
    token_to_market_[token_id] = market_id;

    double new_total = total_exposure_.load() + notional;
    total_exposure_.store(new_total);
    open_positions_++;

    spdlog::debug("Position opened: market={}, token={}, notional=${:.2f}, total=${:.2f}",
                  market_id, token_id, notional, new_total);
}

void ExposureManager::record_position_increased(
    const std::string& market_id,
    const std::string& token_id,
    double additional_notional
) {
    std::lock_guard<std::mutex> lock(mutex_);

    market_exposures_[market_id] += additional_notional;
    position_exposures_[token_id] += additional_notional;

    double new_total = total_exposure_.load() + additional_notional;
    total_exposure_.store(new_total);

    spdlog::debug("Position increased: market={}, token={}, added=${:.2f}, total=${:.2f}",
                  market_id, token_id, additional_notional, new_total);
}

void ExposureManager::record_position_decreased(
    const std::string& market_id,
    const std::string& token_id,
    double reduced_notional
) {
    std::lock_guard<std::mutex> lock(mutex_);

    market_exposures_[market_id] = std::max(0.0, market_exposures_[market_id] - reduced_notional);
    position_exposures_[token_id] = std::max(0.0, position_exposures_[token_id] - reduced_notional);

    double new_total = std::max(0.0, total_exposure_.load() - reduced_notional);
    total_exposure_.store(new_total);

    spdlog::debug("Position decreased: market={}, token={}, reduced=${:.2f}, total=${:.2f}",
                  market_id, token_id, reduced_notional, new_total);
}

void ExposureManager::record_position_closed(
    const std::string& market_id,
    const std::string& token_id
) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto pos_it = position_exposures_.find(token_id);
    if (pos_it == position_exposures_.end()) {
        spdlog::warn("Attempted to close unknown position: {}", token_id);
        return;
    }

    double notional = pos_it->second;

    market_exposures_[market_id] = std::max(0.0, market_exposures_[market_id] - notional);
    position_exposures_.erase(pos_it);

    auto count_it = market_position_counts_.find(market_id);
    if (count_it != market_position_counts_.end()) {
        count_it->second = std::max(0, count_it->second - 1);
        if (count_it->second == 0) {
            market_position_counts_.erase(count_it);
            market_exposures_.erase(market_id);
        }
    }

    token_to_market_.erase(token_id);

    double new_total = std::max(0.0, total_exposure_.load() - notional);
    total_exposure_.store(new_total);
    open_positions_ = std::max(0, open_positions_.load() - 1);

    spdlog::debug("Position closed: market={}, token={}, released=${:.2f}, total=${:.2f}",
                  market_id, token_id, notional, new_total);
}

double ExposureManager::total_exposure() const {
    return total_exposure_.load();
}

double ExposureManager::market_exposure(const std::string& market_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = market_exposures_.find(market_id);
    return (it != market_exposures_.end()) ? it->second : 0.0;
}

double ExposureManager::position_exposure(const std::string& token_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = position_exposures_.find(token_id);
    return (it != position_exposures_.end()) ? it->second : 0.0;
}

int ExposureManager::open_position_count() const {
    return open_positions_.load();
}

int ExposureManager::positions_in_market(const std::string& market_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = market_position_counts_.find(market_id);
    return (it != market_position_counts_.end()) ? it->second : 0;
}

double ExposureManager::total_headroom() const {
    return std::max(0.0, soft_limits_.max_total_exposure - total_exposure_.load());
}

double ExposureManager::market_headroom(const std::string& market_id) const {
    return std::max(0.0, soft_limits_.max_market_exposure - market_exposure(market_id));
}

double ExposureManager::position_headroom(const std::string& token_id) const {
    return std::max(0.0, soft_limits_.max_position_size - position_exposure(token_id));
}

double ExposureManager::total_utilization() const {
    return total_exposure_.load() / soft_limits_.max_total_exposure;
}

double ExposureManager::market_utilization(const std::string& market_id) const {
    return market_exposure(market_id) / soft_limits_.max_market_exposure;
}

void ExposureManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    total_exposure_.store(0.0);
    open_positions_.store(0);
    market_exposures_.clear();
    position_exposures_.clear();
    market_position_counts_.clear();
    token_to_market_.clear();
    spdlog::info("ExposureManager reset");
}

void ExposureManager::load_positions(
    const std::map<std::string, double>& market_exposures,
    const std::map<std::string, double>& position_exposures
) {
    std::lock_guard<std::mutex> lock(mutex_);

    market_exposures_ = market_exposures;
    position_exposures_ = position_exposures;

    double total = 0.0;
    for (const auto& [market, exp] : market_exposures_) {
        total += exp;
    }
    total_exposure_.store(total);
    open_positions_.store(static_cast<int>(position_exposures_.size()));

    // Rebuild position counts
    market_position_counts_.clear();
    for (const auto& [token, exp] : position_exposures_) {
        // Find market for this token
        for (const auto& [market, market_exp] : market_exposures_) {
            // Simplified: assume first matching market
            market_position_counts_[market]++;
            token_to_market_[token] = market;
            break;
        }
    }

    spdlog::info("ExposureManager loaded: total=${:.2f}, positions={}",
                 total, open_positions_.load());
}

// ExposureReservation implementation
ExposureReservation::ExposureReservation(
    ExposureManager& manager,
    const std::string& market_id,
    const std::string& token_id,
    double notional
)
    : manager_(&manager)
    , market_id_(market_id)
    , token_id_(token_id)
    , notional_(notional)
{
    auto result = manager_->can_open_position(market_id, notional);
    if (result.allowed) {
        manager_->record_position_opened(market_id, token_id, notional);
        valid_ = true;
    }
}

ExposureReservation::~ExposureReservation() {
    if (valid_ && !committed_) {
        release();
    }
}

void ExposureReservation::commit() {
    committed_ = true;
}

void ExposureReservation::release() {
    if (valid_ && !committed_) {
        manager_->record_position_closed(market_id_, token_id_);
        valid_ = false;
    }
}

ExposureReservation::ExposureReservation(ExposureReservation&& other) noexcept
    : manager_(other.manager_)
    , market_id_(std::move(other.market_id_))
    , token_id_(std::move(other.token_id_))
    , notional_(other.notional_)
    , valid_(other.valid_)
    , committed_(other.committed_)
{
    other.valid_ = false;
}

ExposureReservation& ExposureReservation::operator=(ExposureReservation&& other) noexcept {
    if (this != &other) {
        if (valid_ && !committed_) {
            release();
        }
        manager_ = other.manager_;
        market_id_ = std::move(other.market_id_);
        token_id_ = std::move(other.token_id_);
        notional_ = other.notional_;
        valid_ = other.valid_;
        committed_ = other.committed_;
        other.valid_ = false;
    }
    return *this;
}

} // namespace arb
