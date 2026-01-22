#pragma once

#include <atomic>
#include <mutex>
#include <map>
#include <string>
#include <optional>
#include "common/types.hpp"

namespace arb {

/**
 * Exposure check result.
 */
struct ExposureCheckResult {
    bool allowed{false};
    std::string rejection_reason;
    double current_exposure{0.0};
    double limit{0.0};
    double headroom{0.0};  // How much more can be added

    explicit operator bool() const { return allowed; }
};

/**
 * Exposure manager enforces hard position limits.
 *
 * DESIGN PRINCIPLES:
 * - Hard limits are COMPILE-TIME constants (cannot be changed via config)
 * - Soft limits come from config but are always <= hard limits
 * - All exposure checks are O(1) via atomic counters
 * - Thread-safe for concurrent access
 *
 * HARD LIMITS (cannot be bypassed):
 * - Max total exposure: $10,000
 * - Max per-market exposure: $2,000
 * - Max open positions: 20
 * - Max position size: $1,000
 */
class ExposureManager {
public:
    // HARD LIMITS - Compile-time constants, cannot be overridden
    static constexpr double HARD_MAX_TOTAL_EXPOSURE = 10000.0;
    static constexpr double HARD_MAX_MARKET_EXPOSURE = 2000.0;
    static constexpr double HARD_MAX_POSITION_SIZE = 1000.0;
    static constexpr int HARD_MAX_OPEN_POSITIONS = 20;
    static constexpr int HARD_MAX_POSITIONS_PER_MARKET = 4;

    struct SoftLimits {
        double max_total_exposure{100.0};      // Default $100
        double max_market_exposure{50.0};       // Default $50 per market
        double max_position_size{10.0};         // Default $10 per position
        int max_open_positions{5};              // Default 5 positions
        int max_positions_per_market{2};        // Default 2 per market

        // Validate soft limits don't exceed hard limits
        bool validate() const;
        void clamp_to_hard_limits();
    };

    explicit ExposureManager(const SoftLimits& limits = SoftLimits{});

    // Pre-trade exposure checks
    ExposureCheckResult can_open_position(
        const std::string& market_id,
        double notional
    ) const;

    ExposureCheckResult can_increase_position(
        const std::string& market_id,
        const std::string& token_id,
        double additional_notional
    ) const;

    // Position tracking
    void record_position_opened(
        const std::string& market_id,
        const std::string& token_id,
        double notional
    );

    void record_position_increased(
        const std::string& market_id,
        const std::string& token_id,
        double additional_notional
    );

    void record_position_decreased(
        const std::string& market_id,
        const std::string& token_id,
        double reduced_notional
    );

    void record_position_closed(
        const std::string& market_id,
        const std::string& token_id
    );

    // Exposure queries
    double total_exposure() const;
    double market_exposure(const std::string& market_id) const;
    double position_exposure(const std::string& token_id) const;
    int open_position_count() const;
    int positions_in_market(const std::string& market_id) const;

    // Headroom queries (how much more can we add)
    double total_headroom() const;
    double market_headroom(const std::string& market_id) const;
    double position_headroom(const std::string& token_id) const;

    // Limit queries
    SoftLimits soft_limits() const { return soft_limits_; }

    // Utilization (0.0 to 1.0)
    double total_utilization() const;
    double market_utilization(const std::string& market_id) const;

    // Reset (for testing/daily reset)
    void reset();

    // Bulk state load (for reconciliation)
    void load_positions(const std::map<std::string, double>& market_exposures,
                        const std::map<std::string, double>& position_exposures);

private:
    SoftLimits soft_limits_;

    mutable std::mutex mutex_;
    std::atomic<double> total_exposure_{0.0};
    std::atomic<int> open_positions_{0};

    std::map<std::string, double> market_exposures_;       // market_id -> exposure
    std::map<std::string, double> position_exposures_;     // token_id -> exposure
    std::map<std::string, int> market_position_counts_;    // market_id -> count
    std::map<std::string, std::string> token_to_market_;   // token_id -> market_id

    // Internal limit checks
    bool check_hard_limits(double new_total, double new_market, double position_size, int new_count) const;
    bool check_soft_limits(double new_total, double new_market, double position_size, int new_count) const;
};

/**
 * RAII guard that reserves exposure before order submission.
 * If order fails, exposure is automatically released.
 */
class ExposureReservation {
public:
    ExposureReservation(
        ExposureManager& manager,
        const std::string& market_id,
        const std::string& token_id,
        double notional
    );
    ~ExposureReservation();

    bool is_valid() const { return valid_; }
    void commit();  // Order succeeded, keep the exposure
    void release(); // Order failed, release the exposure

    // Non-copyable
    ExposureReservation(const ExposureReservation&) = delete;
    ExposureReservation& operator=(const ExposureReservation&) = delete;

    // Movable
    ExposureReservation(ExposureReservation&& other) noexcept;
    ExposureReservation& operator=(ExposureReservation&& other) noexcept;

private:
    ExposureManager* manager_;
    std::string market_id_;
    std::string token_id_;
    double notional_;
    bool valid_{false};
    bool committed_{false};
};

} // namespace arb
