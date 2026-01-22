#include "utils/alerter.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <random>
#include <iomanip>
#include <sstream>
#include <iostream>

namespace arb {

// Alert implementation
std::string Alert::to_string() const {
    std::ostringstream ss;
    auto time = std::chrono::system_clock::to_time_t(timestamp);
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    ss << " [" << severity_to_string(severity) << "]";
    ss << " [" << category_to_string(category) << "]";
    ss << " " << title << ": " << message;
    return ss.str();
}

std::string Alert::to_json() const {
    nlohmann::json j;
    j["id"] = id;
    j["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()).count();
    j["severity"] = severity_to_string(severity);
    j["category"] = category_to_string(category);
    j["title"] = title;
    j["message"] = message;
    j["metadata"] = metadata;
    return j.dump();
}

// LogFileChannel implementation
LogFileChannel::LogFileChannel(const std::string& path)
    : path_(path)
{
    file_.open(path, std::ios::app);
    if (!file_) {
        spdlog::error("Failed to open alert log file: {}", path);
    }
}

LogFileChannel::~LogFileChannel() {
    if (file_.is_open()) {
        file_.close();
    }
}

bool LogFileChannel::send(const Alert& alert) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!file_.is_open()) {
        return false;
    }

    std::string line = alert.to_json() + "\n";
    file_ << line;
    file_.flush();

    bytes_written_ += line.size();
    if (bytes_written_ >= MAX_FILE_SIZE) {
        rotate();
    }

    return true;
}

void LogFileChannel::rotate() {
    file_.close();

    // Rename current file
    std::string backup = path_ + ".1";
    std::rename(path_.c_str(), backup.c_str());

    // Open new file
    file_.open(path_, std::ios::app);
    bytes_written_ = 0;
}

// ConsoleChannel implementation
ConsoleChannel::ConsoleChannel(bool use_colors)
    : use_colors_(use_colors)
{
}

bool ConsoleChannel::send(const Alert& alert) {
    std::string color = use_colors_ ? color_for_severity(alert.severity) : "";
    std::string reset = use_colors_ ? "\033[0m" : "";

    std::cout << color << alert.to_string() << reset << std::endl;
    return true;
}

std::string ConsoleChannel::color_for_severity(AlertSeverity s) const {
    switch (s) {
        case AlertSeverity::INFO: return "\033[36m";     // Cyan
        case AlertSeverity::WARNING: return "\033[33m";  // Yellow
        case AlertSeverity::ERROR: return "\033[31m";    // Red
        case AlertSeverity::CRITICAL: return "\033[1;31m"; // Bold Red
    }
    return "";
}

// WebhookChannel implementation
WebhookChannel::WebhookChannel(const Config& config)
    : config_(config)
{
}

bool WebhookChannel::send(const Alert& alert) {
    if (alert.severity < config_.min_severity) {
        return true;  // Filtered out, but not a failure
    }

    std::string payload = format_generic(alert);

    for (int retry = 0; retry < config_.max_retries; ++retry) {
        if (send_http(payload)) {
            consecutive_failures_.store(0);
            return true;
        }

        // Exponential backoff
        std::this_thread::sleep_for(std::chrono::seconds(1 << retry));
    }

    consecutive_failures_++;
    if (consecutive_failures_.load() >= 5) {
        available_.store(false);
        spdlog::error("Webhook channel disabled after {} failures", consecutive_failures_.load());
    }

    return false;
}

bool WebhookChannel::is_available() const {
    return available_.load();
}

std::string WebhookChannel::format_slack(const Alert& alert) const {
    nlohmann::json j;

    std::string emoji;
    switch (alert.severity) {
        case AlertSeverity::INFO: emoji = ":information_source:"; break;
        case AlertSeverity::WARNING: emoji = ":warning:"; break;
        case AlertSeverity::ERROR: emoji = ":x:"; break;
        case AlertSeverity::CRITICAL: emoji = ":rotating_light:"; break;
    }

    j["text"] = fmt::format("{} *{}* [{}]\n{}", emoji, alert.title,
                           severity_to_string(alert.severity), alert.message);

    return j.dump();
}

std::string WebhookChannel::format_discord(const Alert& alert) const {
    nlohmann::json j;

    int color;
    switch (alert.severity) {
        case AlertSeverity::INFO: color = 0x3498db; break;      // Blue
        case AlertSeverity::WARNING: color = 0xf39c12; break;   // Yellow
        case AlertSeverity::ERROR: color = 0xe74c3c; break;     // Red
        case AlertSeverity::CRITICAL: color = 0x9b59b6; break;  // Purple
    }

    j["embeds"] = nlohmann::json::array();
    j["embeds"][0]["title"] = alert.title;
    j["embeds"][0]["description"] = alert.message;
    j["embeds"][0]["color"] = color;

    return j.dump();
}

std::string WebhookChannel::format_telegram(const Alert& alert) const {
    nlohmann::json j;

    std::string emoji;
    switch (alert.severity) {
        case AlertSeverity::INFO: emoji = "i"; break;
        case AlertSeverity::WARNING: emoji = "!"; break;
        case AlertSeverity::ERROR: emoji = "X"; break;
        case AlertSeverity::CRITICAL: emoji = "!!!"; break;
    }

    j["text"] = fmt::format("[{}] *{}*\n{}", emoji, alert.title, alert.message);
    j["parse_mode"] = "Markdown";

    return j.dump();
}

std::string WebhookChannel::format_generic(const Alert& alert) const {
    return alert.to_json();
}

bool WebhookChannel::send_http(const std::string& payload) {
    // Simplified HTTP POST - in production, use libcurl or similar
    // For now, just log the intent
    spdlog::debug("Webhook send to {}: {}", config_.url, payload.substr(0, 100));

    // TODO: Implement actual HTTP POST
    // For now, simulate success
    return true;
}

// AlertRateLimiter implementation
AlertRateLimiter::AlertRateLimiter(const Config& config)
    : config_(config)
{
}

bool AlertRateLimiter::should_send(const Alert& alert) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now_time = wall_now();
    std::string key = alert_key(alert);

    // Check cooldown
    auto cooldown_it = cooldowns_.find(key);
    if (cooldown_it != cooldowns_.end() && now_time < cooldown_it->second) {
        return false;  // Still in cooldown
    }

    // Check global rate
    auto cutoff = now_time - std::chrono::minutes(1);
    while (!recent_alerts_.empty() && recent_alerts_.front() < cutoff) {
        recent_alerts_.pop_front();
    }
    if (static_cast<int>(recent_alerts_.size()) >= config_.max_alerts_per_minute) {
        return false;
    }

    // Check per-title rate
    auto& title_history = alerts_by_title_[key];
    auto hour_cutoff = now_time - std::chrono::hours(1);
    while (!title_history.empty() && title_history.front() < hour_cutoff) {
        title_history.pop_front();
    }
    if (static_cast<int>(title_history.size()) >= config_.max_same_alert_per_hour) {
        // Set cooldown
        cooldowns_[key] = now_time + config_.cooldown_duration;
        return false;
    }

    return true;
}

void AlertRateLimiter::record_sent(const Alert& alert) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now_time = wall_now();
    std::string key = alert_key(alert);

    recent_alerts_.push_back(now_time);
    alerts_by_title_[key].push_back(now_time);
}

void AlertRateLimiter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_alerts_.clear();
    alerts_by_title_.clear();
    cooldowns_.clear();
}

std::string AlertRateLimiter::alert_key(const Alert& alert) const {
    return fmt::format("{}:{}", category_to_string(alert.category), alert.title);
}

// Alerter implementation
Alerter::Alerter(const Config& config)
    : config_(config)
{
}

Alerter::~Alerter() {
    stop();
}

void Alerter::add_channel(std::shared_ptr<AlertChannel> channel) {
    add_channel(channel, AlertSeverity::INFO);
}

void Alerter::add_channel(std::shared_ptr<AlertChannel> channel, AlertSeverity min_severity) {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    channels_.push_back({channel, min_severity});
    spdlog::info("Added alert channel: {} (min_severity={})",
                 channel->name(), severity_to_string(min_severity));
}

void Alerter::send(const Alert& alert) {
    if (alert.severity < config_.min_severity) {
        return;
    }

    if (config_.deduplicate && should_dedupe(alert)) {
        alerts_dropped_++;
        return;
    }

    if (!rate_limiter_.should_send(alert)) {
        alerts_dropped_++;
        return;
    }

    Alert alert_copy = alert;
    if (alert_copy.id.empty()) {
        alert_copy.id = generate_alert_id();
    }
    if (alert_copy.timestamp.time_since_epoch().count() == 0) {
        alert_copy.timestamp = wall_now();
    }

    if (config_.async_send && running_.load()) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() < static_cast<size_t>(config_.queue_size)) {
            queue_.push(alert_copy);
            queue_cv_.notify_one();
            rate_limiter_.record_sent(alert_copy);
        } else {
            alerts_dropped_++;
        }
    } else {
        send_to_channels(alert_copy);
        rate_limiter_.record_sent(alert_copy);
    }
}

void Alerter::send(AlertSeverity severity, AlertCategory category,
                   const std::string& title, const std::string& message) {
    Alert alert;
    alert.severity = severity;
    alert.category = category;
    alert.title = title;
    alert.message = message;
    send(alert);
}

void Alerter::info(const std::string& title, const std::string& message) {
    send(AlertSeverity::INFO, AlertCategory::SYSTEM, title, message);
}

void Alerter::warning(const std::string& title, const std::string& message) {
    send(AlertSeverity::WARNING, AlertCategory::SYSTEM, title, message);
}

void Alerter::error(const std::string& title, const std::string& message) {
    send(AlertSeverity::ERROR, AlertCategory::SYSTEM, title, message);
}

void Alerter::critical(const std::string& title, const std::string& message) {
    send(AlertSeverity::CRITICAL, AlertCategory::SYSTEM, title, message);
}

void Alerter::trading_alert(AlertSeverity severity, const std::string& title, const std::string& message) {
    send(severity, AlertCategory::TRADING, title, message);
}

void Alerter::risk_alert(AlertSeverity severity, const std::string& title, const std::string& message) {
    send(severity, AlertCategory::RISK, title, message);
}

void Alerter::connectivity_alert(AlertSeverity severity, const std::string& title, const std::string& message) {
    send(severity, AlertCategory::CONNECTIVITY, title, message);
}

void Alerter::pnl_alert(AlertSeverity severity, const std::string& title, const std::string& message) {
    send(severity, AlertCategory::PNL, title, message);
}

void Alerter::start() {
    if (running_.load()) return;

    running_.store(true);
    worker_thread_ = std::thread(&Alerter::worker_loop, this);
    spdlog::info("Alerter started");
}

void Alerter::stop() {
    if (!running_.load()) return;

    running_.store(false);
    queue_cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    spdlog::info("Alerter stopped: sent={}, dropped={}", alerts_sent_.load(), alerts_dropped_.load());
}

void Alerter::flush() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (queue_.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

size_t Alerter::queue_size() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex_));
    return queue_.size();
}

std::string Alerter::generate_alert_id() {
    static std::atomic<int> counter{0};
    return fmt::format("ALT-{}-{}", now_ms(), counter++);
}

bool Alerter::should_dedupe(const Alert& alert) {
    std::lock_guard<std::mutex> lock(dedup_mutex_);

    std::string key = fmt::format("{}:{}:{}", severity_to_string(alert.severity),
                                  category_to_string(alert.category), alert.title);

    auto now_time = wall_now();
    auto it = recent_alerts_.find(key);

    if (it != recent_alerts_.end()) {
        if (now_time - it->second < config_.dedup_window) {
            return true;  // Duplicate
        }
    }

    recent_alerts_[key] = now_time;

    // Cleanup old entries
    auto cutoff = now_time - config_.dedup_window;
    for (auto iter = recent_alerts_.begin(); iter != recent_alerts_.end(); ) {
        if (iter->second < cutoff) {
            iter = recent_alerts_.erase(iter);
        } else {
            ++iter;
        }
    }

    return false;
}

void Alerter::worker_loop() {
    while (running_.load()) {
        Alert alert;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(100),
                              [this] { return !queue_.empty() || !running_.load(); });

            if (!running_.load() && queue_.empty()) break;
            if (queue_.empty()) continue;

            alert = queue_.front();
            queue_.pop();
        }

        send_to_channels(alert);
    }

    // Drain remaining
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!queue_.empty()) {
        send_to_channels(queue_.front());
        queue_.pop();
    }
}

void Alerter::send_to_channels(const Alert& alert) {
    std::lock_guard<std::mutex> lock(channels_mutex_);

    for (const auto& cc : channels_) {
        if (alert.severity < cc.min_severity) continue;
        if (!cc.channel->is_available()) continue;

        try {
            if (cc.channel->send(alert)) {
                alerts_sent_++;
            }
        } catch (const std::exception& e) {
            spdlog::error("Alert channel {} error: {}", cc.channel->name(), e.what());
        }
    }
}

// Global alerter
static std::shared_ptr<Alerter> g_alerter;
static std::mutex g_alerter_mutex;

Alerter& global_alerter() {
    std::lock_guard<std::mutex> lock(g_alerter_mutex);
    if (!g_alerter) {
        g_alerter = std::make_shared<Alerter>();
    }
    return *g_alerter;
}

void set_global_alerter(std::shared_ptr<Alerter> alerter) {
    std::lock_guard<std::mutex> lock(g_alerter_mutex);
    g_alerter = std::move(alerter);
}

} // namespace arb
