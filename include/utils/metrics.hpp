#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <algorithm>
#include "common/types.hpp"

namespace arb {

/**
 * Histogram for latency measurements.
 */
class LatencyHistogram {
public:
    explicit LatencyHistogram(const std::string& name, size_t max_samples = 10000);

    void record(Duration d);
    void record_ns(int64_t ns);

    Duration p50() const;
    Duration p95() const;
    Duration p99() const;
    Duration min() const;
    Duration max() const;
    Duration mean() const;

    int64_t count() const { return count_.load(); }
    void reset();

    std::string summary() const;

private:
    std::string name_;
    size_t max_samples_;
    std::atomic<int64_t> count_{0};

    mutable std::mutex mutex_;
    std::vector<int64_t> samples_ns_;

    int64_t percentile(double p) const;
};

/**
 * Counter metric.
 */
class Counter {
public:
    explicit Counter(const std::string& name);

    void increment(int64_t delta = 1);
    int64_t value() const { return value_.load(); }
    void reset();

    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::atomic<int64_t> value_{0};
};

/**
 * Gauge metric (can go up or down).
 */
class Gauge {
public:
    explicit Gauge(const std::string& name);

    void set(double value);
    void increment(double delta = 1.0);
    void decrement(double delta = 1.0);
    double value() const { return value_.load(); }

    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::atomic<double> value_{0.0};
};

/**
 * Metrics registry for all bot metrics.
 */
class MetricsRegistry {
public:
    static MetricsRegistry& instance();

    // Get or create metrics
    Counter& counter(const std::string& name);
    Gauge& gauge(const std::string& name);
    LatencyHistogram& histogram(const std::string& name);

    // Export all metrics as JSON
    std::string to_json() const;

    // Reset all metrics
    void reset_all();

private:
    MetricsRegistry() = default;

    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Counter>> counters_;
    std::map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::map<std::string, std::unique_ptr<LatencyHistogram>> histograms_;
};

// Convenience macros
#define METRIC_COUNTER(name) MetricsRegistry::instance().counter(name)
#define METRIC_GAUGE(name) MetricsRegistry::instance().gauge(name)
#define METRIC_HISTOGRAM(name) MetricsRegistry::instance().histogram(name)

/**
 * Scoped latency measurement that records to histogram on destruction.
 */
class ScopedLatency {
public:
    ScopedLatency(LatencyHistogram& histogram);
    ~ScopedLatency();

    void stop();  // Early stop
    Duration elapsed() const;

private:
    LatencyHistogram& histogram_;
    Timestamp start_;
    bool stopped_{false};
};

} // namespace arb
