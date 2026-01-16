#include "utils/time_utils.hpp"
#include <sstream>
#include <iomanip>
#include <ctime>

namespace arb {
namespace time_utils {

std::string to_iso8601(WallClock t) {
    auto time_t = std::chrono::system_clock::to_time_t(t);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t.time_since_epoch()) % 1000;

    std::tm tm = *std::gmtime(&time_t);

    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';

    return ss.str();
}

std::string to_iso8601(int64_t epoch_ms) {
    auto tp = WallClock(std::chrono::milliseconds(epoch_ms));
    return to_iso8601(tp);
}

WallClock from_iso8601(const std::string& s) {
    std::tm tm = {};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

    auto time_t = timegm(&tm);
    auto tp = std::chrono::system_clock::from_time_t(time_t);

    // Parse milliseconds if present
    size_t dot_pos = s.find('.');
    if (dot_pos != std::string::npos && dot_pos + 1 < s.length()) {
        std::string ms_str = s.substr(dot_pos + 1, 3);
        int ms = std::stoi(ms_str);
        tp += std::chrono::milliseconds(ms);
    }

    return tp;
}

std::string now_iso8601() {
    return to_iso8601(wall_now());
}

int64_t epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

int64_t epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string format_duration(Duration d) {
    auto ns = d.count();

    if (ns < 1000) {
        return std::to_string(ns) + "ns";
    } else if (ns < 1000000) {
        return std::to_string(ns / 1000) + "us";
    } else if (ns < 1000000000) {
        return std::to_string(ns / 1000000) + "ms";
    } else {
        double sec = ns / 1e9;
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2) << sec << "s";
        return ss.str();
    }
}

std::string format_duration_ms(int64_t ms) {
    if (ms < 1000) {
        return std::to_string(ms) + "ms";
    } else if (ms < 60000) {
        double sec = ms / 1000.0;
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << sec << "s";
        return ss.str();
    } else {
        int min = ms / 60000;
        int sec = (ms % 60000) / 1000;
        return std::to_string(min) + "m" + std::to_string(sec) + "s";
    }
}

bool is_trading_hours() {
    // Crypto markets are 24/7
    return true;
}

Duration time_to_next_15m() {
    auto now_tp = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now_tp);
    std::tm tm = *std::gmtime(&now_time);

    // Calculate minutes until next 15-minute boundary
    int current_min = tm.tm_min;
    int next_15m = ((current_min / 15) + 1) * 15;

    if (next_15m >= 60) {
        next_15m = 0;
    }

    int mins_to_wait = (next_15m - current_min + 60) % 60;
    if (mins_to_wait == 0) mins_to_wait = 15;

    // Subtract current seconds
    int secs_to_wait = mins_to_wait * 60 - tm.tm_sec;

    return std::chrono::seconds(secs_to_wait);
}

Duration time_until(WallClock target) {
    auto now_tp = std::chrono::system_clock::now();
    if (target <= now_tp) {
        return Duration::zero();
    }
    return std::chrono::duration_cast<Duration>(target - now_tp);
}

// LatencyTimer implementation

LatencyTimer::LatencyTimer()
    : start_(now())
    , end_(start_)
{
}

void LatencyTimer::start() {
    start_ = now();
    running_ = true;
}

void LatencyTimer::stop() {
    end_ = now();
    running_ = false;
}

void LatencyTimer::reset() {
    start_ = now();
    end_ = start_;
    running_ = false;
}

Duration LatencyTimer::elapsed() const {
    if (running_) {
        return now() - start_;
    }
    return end_ - start_;
}

int64_t LatencyTimer::elapsed_ns() const {
    return elapsed().count();
}

int64_t LatencyTimer::elapsed_us() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(elapsed()).count();
}

int64_t LatencyTimer::elapsed_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed()).count();
}

// RateLimiter implementation

RateLimiter::RateLimiter(int max_requests, int window_seconds)
    : max_requests_(max_requests)
    , window_seconds_(window_seconds)
    , window_start_(std::chrono::steady_clock::now())
{
}

bool RateLimiter::try_acquire() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now_tp = std::chrono::steady_clock::now();
    auto window_duration = std::chrono::seconds(window_seconds_);

    // Reset window if expired
    if (now_tp - window_start_ >= window_duration) {
        window_start_ = now_tp;
        requests_in_window_ = 0;
    }

    if (requests_in_window_ >= max_requests_) {
        return false;
    }

    requests_in_window_++;
    return true;
}

void RateLimiter::acquire() {
    while (!try_acquire()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int RateLimiter::remaining() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::max(0, max_requests_ - requests_in_window_);
}

void RateLimiter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    window_start_ = std::chrono::steady_clock::now();
    requests_in_window_ = 0;
}

} // namespace time_utils
} // namespace arb
