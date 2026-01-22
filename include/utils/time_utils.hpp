#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <thread>
#include "common/types.hpp"

namespace arb {
namespace time_utils {

/**
 * Convert timestamp to ISO 8601 string.
 */
std::string to_iso8601(WallClock t);
std::string to_iso8601(int64_t epoch_ms);

/**
 * Parse ISO 8601 string to timestamp.
 */
WallClock from_iso8601(const std::string& s);

/**
 * Get current timestamp as ISO 8601.
 */
std::string now_iso8601();

/**
 * Get current epoch milliseconds.
 */
int64_t epoch_ms();

/**
 * Get current epoch seconds.
 */
int64_t epoch_seconds();

/**
 * Format duration for display.
 */
std::string format_duration(Duration d);
std::string format_duration_ms(int64_t ms);

/**
 * Check if time is within trading hours (always true for crypto).
 */
bool is_trading_hours();

/**
 * Calculate time until next 15-minute interval.
 */
Duration time_to_next_15m();

/**
 * Calculate time until specific wall clock.
 */
Duration time_until(WallClock target);

/**
 * High resolution timer for latency measurement.
 */
class LatencyTimer {
public:
    LatencyTimer();

    void start();
    void stop();
    void reset();

    Duration elapsed() const;
    int64_t elapsed_ns() const;
    int64_t elapsed_us() const;
    int64_t elapsed_ms() const;

private:
    Timestamp start_;
    Timestamp end_;
    bool running_{false};
};

/**
 * Rate limiter for API calls.
 */
class RateLimiter {
public:
    RateLimiter(int max_requests, int window_seconds);

    // Returns true if request is allowed, false if rate limited
    bool try_acquire();

    // Wait until request is allowed
    void acquire();

    // Get remaining requests in current window
    int remaining() const;

    // Reset the rate limiter
    void reset();

private:
    int max_requests_;
    int window_seconds_;
    std::chrono::steady_clock::time_point window_start_;
    int requests_in_window_{0};
    mutable std::mutex mutex_;
};

} // namespace time_utils
} // namespace arb
