#include "core/connection_health.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <numeric>

namespace arb {

double ConnectionMetrics::message_rate() const {
    // Simple approximation
    auto age = std::chrono::duration_cast<std::chrono::seconds>(wall_now() - last_connected);
    if (age.count() <= 0) return 0.0;
    return static_cast<double>(messages_received) / age.count();
}

double ConnectionMetrics::error_rate() const {
    if (messages_received == 0) return 0.0;
    return static_cast<double>(errors) / static_cast<double>(messages_received);
}

double ConnectionMetrics::availability() const {
    // Would need more tracking for accurate calculation
    return connected ? 1.0 : 0.0;
}

bool SystemHealth::all_healthy() const {
    for (const auto& [name, metrics] : connections) {
        if (metrics.status != HealthStatus::HEALTHY) {
            return false;
        }
    }
    return !connections.empty();
}

bool SystemHealth::any_disconnected() const {
    for (const auto& [name, metrics] : connections) {
        if (metrics.status == HealthStatus::DISCONNECTED) {
            return true;
        }
    }
    return false;
}

bool SystemHealth::can_trade() const {
    return overall_status == HealthStatus::HEALTHY ||
           overall_status == HealthStatus::DEGRADED;
}

std::vector<std::string> SystemHealth::unhealthy_connections() const {
    std::vector<std::string> result;
    for (const auto& [name, metrics] : connections) {
        if (metrics.status == HealthStatus::UNHEALTHY ||
            metrics.status == HealthStatus::DISCONNECTED) {
            result.push_back(name);
        }
    }
    return result;
}

std::string SystemHealth::summary() const {
    std::string s = fmt::format("System: {} | ", health_status_to_string(overall_status));
    for (const auto& [name, metrics] : connections) {
        s += fmt::format("{}: {} ", name, health_status_to_string(metrics.status));
    }
    return s;
}

// ConnectionHealthMonitor implementation
ConnectionHealthMonitor::ConnectionHealthMonitor(
    const Config& config,
    const RequiredConnections& required
)
    : config_(config)
    , required_(required)
{
    spdlog::info("ConnectionHealthMonitor initialized: heartbeat_interval={}s, timeout={}s",
                 config.heartbeat_interval.count(),
                 config.heartbeat_timeout.count());
}

void ConnectionHealthMonitor::register_connection(const std::string& name, bool is_required) {
    std::lock_guard<std::mutex> lock(mutex_);

    ConnectionMetrics m;
    m.name = name;
    m.status = HealthStatus::UNKNOWN;
    metrics_[name] = m;

    if (is_required) {
        if (std::find(required_.required.begin(), required_.required.end(), name)
            == required_.required.end()) {
            required_.required.push_back(name);
        }
    } else {
        if (std::find(required_.optional.begin(), required_.optional.end(), name)
            == required_.optional.end()) {
            required_.optional.push_back(name);
        }
    }

    spdlog::info("Registered connection: {} (required={})", name, is_required);
}

void ConnectionHealthMonitor::unregister_connection(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.erase(name);
    message_history_.erase(name);

    required_.required.erase(
        std::remove(required_.required.begin(), required_.required.end(), name),
        required_.required.end()
    );
    required_.optional.erase(
        std::remove(required_.optional.begin(), required_.optional.end(), name),
        required_.optional.end()
    );
}

void ConnectionHealthMonitor::record_connected(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metrics_.find(name);
    if (it == metrics_.end()) return;

    auto& m = it->second;
    m.connected = true;
    m.last_connected = wall_now();

    auto old_status = m.status;
    m.status = calculate_health(m);

    if (old_status != m.status) {
        invoke_health_change(name, old_status, m.status);
    }

    spdlog::info("Connection established: {}", name);
}

void ConnectionHealthMonitor::record_disconnected(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metrics_.find(name);
    if (it == metrics_.end()) return;

    auto& m = it->second;
    m.connected = false;
    m.last_disconnected = wall_now();

    // Calculate uptime
    if (m.last_connected.time_since_epoch().count() > 0) {
        m.uptime += std::chrono::duration_cast<Duration>(m.last_disconnected - m.last_connected);
    }

    auto old_status = m.status;
    m.status = HealthStatus::DISCONNECTED;

    if (old_status != m.status) {
        invoke_health_change(name, old_status, m.status);
    }

    spdlog::warn("Connection lost: {}", name);
}

void ConnectionHealthMonitor::record_reconnect(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metrics_.find(name);
    if (it == metrics_.end()) return;

    it->second.reconnect_count++;
    spdlog::info("Reconnect #{} for: {}", it->second.reconnect_count, name);
}

void ConnectionHealthMonitor::record_message_received(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metrics_.find(name);
    if (it == metrics_.end()) return;

    it->second.messages_received++;
    it->second.last_message_time = wall_now();

    message_history_[name].emplace_back(wall_now(), false);
    cleanup_old_history(name);
}

void ConnectionHealthMonitor::record_message_sent(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metrics_.find(name);
    if (it == metrics_.end()) return;

    it->second.messages_sent++;
}

void ConnectionHealthMonitor::record_error(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metrics_.find(name);
    if (it == metrics_.end()) return;

    it->second.errors++;
    message_history_[name].emplace_back(wall_now(), true);
    cleanup_old_history(name);

    spdlog::debug("Error recorded for {}: total errors = {}", name, it->second.errors);
}

void ConnectionHealthMonitor::record_latency(const std::string& name, Duration latency) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metrics_.find(name);
    if (it == metrics_.end()) return;

    auto& m = it->second;
    m.last_latency = latency;

    // Simple exponential moving average
    if (m.avg_latency.count() == 0) {
        m.avg_latency = latency;
    } else {
        m.avg_latency = Duration(static_cast<int64_t>(
            m.avg_latency.count() * 0.9 + latency.count() * 0.1
        ));
    }

    if (latency > m.max_latency) {
        m.max_latency = latency;
    }
}

void ConnectionHealthMonitor::record_heartbeat(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metrics_.find(name);
    if (it == metrics_.end()) return;

    it->second.last_heartbeat = wall_now();
    it->second.heartbeat_active = true;
    it->second.missed_heartbeats = 0;
}

void ConnectionHealthMonitor::check_heartbeats() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now_time = wall_now();

    for (auto& [name, m] : metrics_) {
        if (!m.heartbeat_active) continue;

        auto since_heartbeat = now_time - m.last_heartbeat;
        if (since_heartbeat > config_.heartbeat_timeout) {
            m.missed_heartbeats++;

            if (m.missed_heartbeats >= config_.max_missed_heartbeats) {
                auto old_status = m.status;
                m.status = HealthStatus::UNHEALTHY;

                if (old_status != m.status) {
                    spdlog::warn("Connection {} missed {} heartbeats, marking UNHEALTHY",
                                name, m.missed_heartbeats);
                    invoke_health_change(name, old_status, m.status);
                }
            }
        }
    }
}

HealthStatus ConnectionHealthMonitor::connection_health(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    return (it != metrics_.end()) ? it->second.status : HealthStatus::UNKNOWN;
}

ConnectionMetrics ConnectionHealthMonitor::connection_metrics(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = metrics_.find(name);
    return (it != metrics_.end()) ? it->second : ConnectionMetrics{};
}

SystemHealth ConnectionHealthMonitor::system_health() const {
    std::lock_guard<std::mutex> lock(mutex_);

    SystemHealth health;
    health.connections = metrics_;
    health.overall_status = calculate_system_health();

    return health;
}

bool ConnectionHealthMonitor::is_trading_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // All required connections must be at least DEGRADED
    for (const auto& name : required_.required) {
        auto it = metrics_.find(name);
        if (it == metrics_.end()) return false;
        if (it->second.status == HealthStatus::UNHEALTHY ||
            it->second.status == HealthStatus::DISCONNECTED ||
            it->second.status == HealthStatus::UNKNOWN) {
            return false;
        }
    }

    return true;
}

void ConnectionHealthMonitor::evaluate_health() {
    std::vector<std::pair<std::string, std::pair<HealthStatus, HealthStatus>>> changes;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& [name, m] : metrics_) {
            auto old_status = m.status;
            m.status = calculate_health(m);

            if (old_status != m.status) {
                changes.emplace_back(name, std::make_pair(old_status, m.status));
            }
        }
    }

    // Invoke callbacks outside lock
    for (const auto& [name, statuses] : changes) {
        invoke_health_change(name, statuses.first, statuses.second);
    }

    // Check if system health changed
    auto current_system = calculate_system_health();
    if (current_system != previous_system_status_) {
        previous_system_status_ = current_system;
        if (on_system_health_) {
            on_system_health_(system_health());
        }
    }
}

void ConnectionHealthMonitor::reset_connection(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metrics_.find(name);
    if (it == metrics_.end()) return;

    auto& m = it->second;
    m.messages_received = 0;
    m.messages_sent = 0;
    m.errors = 0;
    m.reconnect_count = 0;
    m.avg_latency = Duration(0);
    m.max_latency = Duration(0);
    m.missed_heartbeats = 0;

    message_history_[name].clear();

    spdlog::info("Reset metrics for: {}", name);
}

void ConnectionHealthMonitor::reset_all() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [name, m] : metrics_) {
        m.messages_received = 0;
        m.messages_sent = 0;
        m.errors = 0;
        m.reconnect_count = 0;
        m.avg_latency = Duration(0);
        m.max_latency = Duration(0);
        m.missed_heartbeats = 0;
    }

    message_history_.clear();
    spdlog::info("Reset all connection metrics");
}

HealthStatus ConnectionHealthMonitor::calculate_health(const ConnectionMetrics& m) const {
    if (!m.connected) {
        return HealthStatus::DISCONNECTED;
    }

    // Check error rate
    double error_rate = m.error_rate();
    if (error_rate >= config_.unhealthy_error_rate) {
        return HealthStatus::UNHEALTHY;
    }

    // Check latency
    if (m.avg_latency >= config_.unhealthy_latency) {
        return HealthStatus::UNHEALTHY;
    }

    // Check for missed heartbeats
    if (m.missed_heartbeats >= config_.max_missed_heartbeats) {
        return HealthStatus::UNHEALTHY;
    }

    // Check for degraded conditions
    if (error_rate >= config_.degraded_error_rate) {
        return HealthStatus::DEGRADED;
    }

    if (m.avg_latency >= config_.degraded_latency) {
        return HealthStatus::DEGRADED;
    }

    if (m.missed_heartbeats > 0) {
        return HealthStatus::DEGRADED;
    }

    return HealthStatus::HEALTHY;
}

HealthStatus ConnectionHealthMonitor::calculate_system_health() const {
    bool any_unhealthy = false;
    bool any_degraded = false;
    bool any_disconnected = false;

    // Check required connections
    for (const auto& name : required_.required) {
        auto it = metrics_.find(name);
        if (it == metrics_.end()) {
            return HealthStatus::UNHEALTHY;
        }

        switch (it->second.status) {
            case HealthStatus::DISCONNECTED:
                any_disconnected = true;
                break;
            case HealthStatus::UNHEALTHY:
                any_unhealthy = true;
                break;
            case HealthStatus::DEGRADED:
                any_degraded = true;
                break;
            default:
                break;
        }
    }

    // Required connection issues are critical
    if (any_disconnected) return HealthStatus::DISCONNECTED;
    if (any_unhealthy) return HealthStatus::UNHEALTHY;

    // Check optional connections (less critical)
    for (const auto& name : required_.optional) {
        auto it = metrics_.find(name);
        if (it != metrics_.end() && it->second.status == HealthStatus::DEGRADED) {
            any_degraded = true;
        }
    }

    if (any_degraded) return HealthStatus::DEGRADED;
    return HealthStatus::HEALTHY;
}

void ConnectionHealthMonitor::update_message_rate(const std::string& name) {
    // Calculated on demand via message_rate()
}

void ConnectionHealthMonitor::cleanup_old_history(const std::string& name) {
    auto& history = message_history_[name];
    auto cutoff = wall_now() - config_.metrics_window;

    history.erase(
        std::remove_if(history.begin(), history.end(),
                       [&cutoff](const auto& entry) { return entry.first < cutoff; }),
        history.end()
    );
}

void ConnectionHealthMonitor::invoke_health_change(
    const std::string& name,
    HealthStatus old_status,
    HealthStatus new_status
) {
    spdlog::info("Health change: {} {} -> {}",
                name,
                health_status_to_string(old_status),
                health_status_to_string(new_status));

    if (on_health_change_) {
        try {
            on_health_change_(name, old_status, new_status);
        } catch (const std::exception& e) {
            spdlog::error("Health change callback error: {}", e.what());
        }
    }
}

} // namespace arb
