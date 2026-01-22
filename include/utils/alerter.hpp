#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <fstream>
#include "common/types.hpp"

namespace arb {

/**
 * Alert severity levels.
 */
enum class AlertSeverity {
    INFO,       // Informational
    WARNING,    // Warning - potential issue
    ERROR,      // Error - something went wrong
    CRITICAL    // Critical - immediate attention needed
};

inline std::string severity_to_string(AlertSeverity s) {
    switch (s) {
        case AlertSeverity::INFO: return "INFO";
        case AlertSeverity::WARNING: return "WARNING";
        case AlertSeverity::ERROR: return "ERROR";
        case AlertSeverity::CRITICAL: return "CRITICAL";
    }
    return "UNKNOWN";
}

/**
 * Alert categories for filtering.
 */
enum class AlertCategory {
    SYSTEM,         // System-level alerts
    TRADING,        // Trading activity
    RISK,           // Risk management
    CONNECTIVITY,   // Connection issues
    PERFORMANCE,    // Performance degradation
    PNL             // P&L related
};

inline std::string category_to_string(AlertCategory c) {
    switch (c) {
        case AlertCategory::SYSTEM: return "SYSTEM";
        case AlertCategory::TRADING: return "TRADING";
        case AlertCategory::RISK: return "RISK";
        case AlertCategory::CONNECTIVITY: return "CONNECTIVITY";
        case AlertCategory::PERFORMANCE: return "PERFORMANCE";
        case AlertCategory::PNL: return "PNL";
    }
    return "UNKNOWN";
}

/**
 * Alert message structure.
 */
struct Alert {
    std::string id;
    WallClock timestamp;
    AlertSeverity severity;
    AlertCategory category;
    std::string title;
    std::string message;
    std::map<std::string, std::string> metadata;

    std::string to_string() const;
    std::string to_json() const;
};

/**
 * Alert channel interface.
 */
class AlertChannel {
public:
    virtual ~AlertChannel() = default;
    virtual bool send(const Alert& alert) = 0;
    virtual std::string name() const = 0;
    virtual bool is_available() const = 0;
};

/**
 * Log file alert channel.
 */
class LogFileChannel : public AlertChannel {
public:
    explicit LogFileChannel(const std::string& path);
    ~LogFileChannel() override;

    bool send(const Alert& alert) override;
    std::string name() const override { return "log_file"; }
    bool is_available() const override { return file_.is_open(); }

    void rotate();

private:
    std::string path_;
    std::ofstream file_;
    std::mutex mutex_;
    size_t bytes_written_{0};
    static constexpr size_t MAX_FILE_SIZE = 50 * 1024 * 1024;  // 50MB
};

/**
 * Console alert channel (colored output).
 */
class ConsoleChannel : public AlertChannel {
public:
    explicit ConsoleChannel(bool use_colors = true);

    bool send(const Alert& alert) override;
    std::string name() const override { return "console"; }
    bool is_available() const override { return true; }

private:
    bool use_colors_;
    std::string color_for_severity(AlertSeverity s) const;
};

/**
 * Webhook alert channel (Slack, Discord, Telegram, etc).
 */
class WebhookChannel : public AlertChannel {
public:
    struct Config {
        std::string url;
        std::string method{"POST"};
        std::map<std::string, std::string> headers;
        std::chrono::seconds timeout{10};
        int max_retries{3};
        AlertSeverity min_severity{AlertSeverity::WARNING};
    };

    explicit WebhookChannel(const Config& config);

    bool send(const Alert& alert) override;
    std::string name() const override { return "webhook"; }
    bool is_available() const override;

    // Format alert for different services
    std::string format_slack(const Alert& alert) const;
    std::string format_discord(const Alert& alert) const;
    std::string format_telegram(const Alert& alert) const;
    std::string format_generic(const Alert& alert) const;

private:
    Config config_;
    std::atomic<bool> available_{true};
    std::atomic<int> consecutive_failures_{0};

    bool send_http(const std::string& payload);
};

/**
 * Rate limiter for alerts.
 */
class AlertRateLimiter {
public:
    struct Config {
        int max_alerts_per_minute{10};
        int max_same_alert_per_hour{5};
        std::chrono::minutes cooldown_duration{15};
    };

    explicit AlertRateLimiter(const Config& config = Config{});

    bool should_send(const Alert& alert);
    void record_sent(const Alert& alert);
    void reset();

private:
    Config config_;
    mutable std::mutex mutex_;

    std::deque<WallClock> recent_alerts_;
    std::map<std::string, std::deque<WallClock>> alerts_by_title_;
    std::map<std::string, WallClock> cooldowns_;

    std::string alert_key(const Alert& alert) const;
};

/**
 * Main alerter class - sends alerts through multiple channels.
 *
 * DESIGN:
 * - Async sending (non-blocking)
 * - Multiple channels (log, console, webhook)
 * - Rate limiting to prevent spam
 * - Severity filtering per channel
 * - Automatic retry for failed sends
 */
class Alerter {
public:
    struct Config {
        bool async_send{true};
        int queue_size{1000};
        AlertSeverity min_severity{AlertSeverity::INFO};
        bool deduplicate{true};
        std::chrono::seconds dedup_window{60};
    };

    explicit Alerter(const Config& config = Config{});
    ~Alerter();

    // Add channels
    void add_channel(std::shared_ptr<AlertChannel> channel);
    void add_channel(std::shared_ptr<AlertChannel> channel, AlertSeverity min_severity);

    // Send alerts
    void send(const Alert& alert);
    void send(AlertSeverity severity, AlertCategory category,
              const std::string& title, const std::string& message);

    // Convenience methods
    void info(const std::string& title, const std::string& message);
    void warning(const std::string& title, const std::string& message);
    void error(const std::string& title, const std::string& message);
    void critical(const std::string& title, const std::string& message);

    // Category-specific alerts
    void trading_alert(AlertSeverity severity, const std::string& title, const std::string& message);
    void risk_alert(AlertSeverity severity, const std::string& title, const std::string& message);
    void connectivity_alert(AlertSeverity severity, const std::string& title, const std::string& message);
    void pnl_alert(AlertSeverity severity, const std::string& title, const std::string& message);

    // Control
    void start();
    void stop();
    void flush();  // Block until queue is empty

    // Stats
    int64_t alerts_sent() const { return alerts_sent_.load(); }
    int64_t alerts_dropped() const { return alerts_dropped_.load(); }
    size_t queue_size() const;

private:
    Config config_;
    AlertRateLimiter rate_limiter_;

    struct ChannelConfig {
        std::shared_ptr<AlertChannel> channel;
        AlertSeverity min_severity;
    };
    std::vector<ChannelConfig> channels_;
    std::mutex channels_mutex_;

    // Async queue
    std::queue<Alert> queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::atomic<bool> running_{false};
    std::thread worker_thread_;

    std::atomic<int64_t> alerts_sent_{0};
    std::atomic<int64_t> alerts_dropped_{0};

    // Deduplication
    std::mutex dedup_mutex_;
    std::map<std::string, WallClock> recent_alerts_;

    std::string generate_alert_id();
    bool should_dedupe(const Alert& alert);
    void worker_loop();
    void send_to_channels(const Alert& alert);
};

/**
 * Global alerter singleton for convenience.
 */
Alerter& global_alerter();
void set_global_alerter(std::shared_ptr<Alerter> alerter);

// Convenience macros
#define ALERT_INFO(title, msg) ::arb::global_alerter().info(title, msg)
#define ALERT_WARN(title, msg) ::arb::global_alerter().warning(title, msg)
#define ALERT_ERROR(title, msg) ::arb::global_alerter().error(title, msg)
#define ALERT_CRITICAL(title, msg) ::arb::global_alerter().critical(title, msg)

} // namespace arb
