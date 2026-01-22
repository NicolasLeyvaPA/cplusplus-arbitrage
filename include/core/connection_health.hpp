#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <functional>
#include <optional>
#include "common/types.hpp"

namespace arb {

/**
 * Health status for a single connection.
 */
enum class HealthStatus {
    HEALTHY,       // All good
    DEGRADED,      // Working but with issues
    UNHEALTHY,     // Not working properly
    DISCONNECTED,  // Not connected
    UNKNOWN        // No data yet
};

inline std::string health_status_to_string(HealthStatus s) {
    switch (s) {
        case HealthStatus::HEALTHY: return "HEALTHY";
        case HealthStatus::DEGRADED: return "DEGRADED";
        case HealthStatus::UNHEALTHY: return "UNHEALTHY";
        case HealthStatus::DISCONNECTED: return "DISCONNECTED";
        default: return "UNKNOWN";
    }
}

/**
 * Metrics for a single connection.
 */
struct ConnectionMetrics {
    std::string name;
    HealthStatus status{HealthStatus::UNKNOWN};

    // Connection state
    bool connected{false};
    int reconnect_count{0};
    WallClock last_connected;
    WallClock last_disconnected;
    Duration uptime{0};

    // Message stats
    int64_t messages_received{0};
    int64_t messages_sent{0};
    int64_t errors{0};
    WallClock last_message_time;

    // Latency (round-trip if available)
    Duration last_latency{0};
    Duration avg_latency{0};
    Duration p99_latency{0};
    Duration max_latency{0};

    // Heartbeat
    bool heartbeat_active{false};
    WallClock last_heartbeat;
    int missed_heartbeats{0};

    // Calculated
    double message_rate() const;  // messages/second over last minute
    double error_rate() const;    // errors/message ratio
    double availability() const;  // uptime percentage
};

/**
 * Overall system health.
 */
struct SystemHealth {
    HealthStatus overall_status{HealthStatus::UNKNOWN};
    std::map<std::string, ConnectionMetrics> connections;

    bool all_healthy() const;
    bool any_disconnected() const;
    bool can_trade() const;  // At least minimum required connections
    std::vector<std::string> unhealthy_connections() const;
    std::string summary() const;
};

/**
 * Connection health monitor tracks all data source connections.
 *
 * DESIGN:
 * - Track multiple named connections (binance, polymarket_ws, polymarket_rest)
 * - Compute health based on heartbeats, latency, error rates
 * - Trigger callbacks when health changes
 * - Support minimum required connections for trading
 */
class ConnectionHealthMonitor {
public:
    using HealthChangeCallback = std::function<void(const std::string&, HealthStatus, HealthStatus)>;
    using SystemHealthCallback = std::function<void(const SystemHealth&)>;

    struct Config {
        std::chrono::seconds heartbeat_interval{30};
        std::chrono::seconds heartbeat_timeout{60};
        int max_missed_heartbeats{3};
        double degraded_error_rate{0.01};   // 1% errors = degraded
        double unhealthy_error_rate{0.05};  // 5% errors = unhealthy
        Duration degraded_latency{std::chrono::milliseconds(500)};
        Duration unhealthy_latency{std::chrono::seconds(2)};
        std::chrono::seconds metrics_window{60};  // Rolling window for rates
    };

    struct RequiredConnections {
        std::vector<std::string> required;   // Must be healthy
        std::vector<std::string> optional;   // Nice to have
    };

    explicit ConnectionHealthMonitor(
        const Config& config = Config{},
        const RequiredConnections& required = RequiredConnections{}
    );

    // Register a connection to monitor
    void register_connection(const std::string& name, bool is_required = true);
    void unregister_connection(const std::string& name);

    // Update connection state
    void record_connected(const std::string& name);
    void record_disconnected(const std::string& name);
    void record_reconnect(const std::string& name);

    // Update message stats
    void record_message_received(const std::string& name);
    void record_message_sent(const std::string& name);
    void record_error(const std::string& name);
    void record_latency(const std::string& name, Duration latency);

    // Heartbeat management
    void record_heartbeat(const std::string& name);
    void check_heartbeats();  // Call periodically

    // Health queries
    HealthStatus connection_health(const std::string& name) const;
    ConnectionMetrics connection_metrics(const std::string& name) const;
    SystemHealth system_health() const;
    bool is_trading_ready() const;

    // Callbacks
    void set_health_change_callback(HealthChangeCallback cb) { on_health_change_ = std::move(cb); }
    void set_system_health_callback(SystemHealthCallback cb) { on_system_health_ = std::move(cb); }

    // Periodic health evaluation (call from main loop or timer)
    void evaluate_health();

    // Reset stats (for testing/daily reset)
    void reset_connection(const std::string& name);
    void reset_all();

private:
    Config config_;
    RequiredConnections required_;

    mutable std::mutex mutex_;
    std::map<std::string, ConnectionMetrics> metrics_;
    std::map<std::string, std::vector<std::pair<WallClock, bool>>> message_history_;  // time, is_error

    HealthChangeCallback on_health_change_;
    SystemHealthCallback on_system_health_;

    HealthStatus previous_system_status_{HealthStatus::UNKNOWN};

    // Internal calculations
    HealthStatus calculate_health(const ConnectionMetrics& m) const;
    HealthStatus calculate_system_health() const;
    void update_message_rate(const std::string& name);
    void cleanup_old_history(const std::string& name);
    void invoke_health_change(const std::string& name, HealthStatus old_status, HealthStatus new_status);
};

/**
 * RAII heartbeat sender for a connection.
 */
class HeartbeatSender {
public:
    HeartbeatSender(ConnectionHealthMonitor& monitor, const std::string& name)
        : monitor_(monitor), name_(name) {
        monitor_.record_heartbeat(name_);
    }

private:
    ConnectionHealthMonitor& monitor_;
    std::string name_;
};

} // namespace arb
