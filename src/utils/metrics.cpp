#include "utils/metrics.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>
#include <numeric>

namespace arb {

// LatencyHistogram implementation

LatencyHistogram::LatencyHistogram(const std::string& name, size_t max_samples)
    : name_(name)
    , max_samples_(max_samples)
{
    samples_ns_.reserve(max_samples);
}

void LatencyHistogram::record(Duration d) {
    record_ns(d.count());
}

void LatencyHistogram::record_ns(int64_t ns) {
    std::lock_guard<std::mutex> lock(mutex_);

    samples_ns_.push_back(ns);
    count_++;

    // Trim if over capacity
    if (samples_ns_.size() > max_samples_) {
        samples_ns_.erase(samples_ns_.begin());
    }
}

int64_t LatencyHistogram::percentile(double p) const {
    // Assumes mutex is held
    if (samples_ns_.empty()) return 0;

    std::vector<int64_t> sorted = samples_ns_;
    std::sort(sorted.begin(), sorted.end());

    size_t idx = static_cast<size_t>((p / 100.0) * (sorted.size() - 1));
    return sorted[idx];
}

Duration LatencyHistogram::p50() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Duration(percentile(50.0));
}

Duration LatencyHistogram::p95() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Duration(percentile(95.0));
}

Duration LatencyHistogram::p99() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Duration(percentile(99.0));
}

Duration LatencyHistogram::min() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_ns_.empty()) return Duration::zero();
    return Duration(*std::min_element(samples_ns_.begin(), samples_ns_.end()));
}

Duration LatencyHistogram::max() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_ns_.empty()) return Duration::zero();
    return Duration(*std::max_element(samples_ns_.begin(), samples_ns_.end()));
}

Duration LatencyHistogram::mean() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_ns_.empty()) return Duration::zero();
    int64_t sum = std::accumulate(samples_ns_.begin(), samples_ns_.end(), 0LL);
    return Duration(sum / samples_ns_.size());
}

void LatencyHistogram::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_ns_.clear();
    count_ = 0;
}

std::string LatencyHistogram::summary() const {
    std::ostringstream ss;
    ss << name_ << ": ";
    ss << "count=" << count_.load();
    ss << " p50=" << std::chrono::duration_cast<std::chrono::microseconds>(p50()).count() << "us";
    ss << " p95=" << std::chrono::duration_cast<std::chrono::microseconds>(p95()).count() << "us";
    ss << " p99=" << std::chrono::duration_cast<std::chrono::microseconds>(p99()).count() << "us";
    return ss.str();
}

// Counter implementation

Counter::Counter(const std::string& name)
    : name_(name)
{
}

void Counter::increment(int64_t delta) {
    value_ += delta;
}

void Counter::reset() {
    value_ = 0;
}

// Gauge implementation

Gauge::Gauge(const std::string& name)
    : name_(name)
{
}

void Gauge::set(double value) {
    value_ = value;
}

void Gauge::increment(double delta) {
    double old = value_.load();
    while (!value_.compare_exchange_weak(old, old + delta)) {}
}

void Gauge::decrement(double delta) {
    increment(-delta);
}

// MetricsRegistry implementation

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry registry;
    return registry;
}

Counter& MetricsRegistry::counter(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = counters_.find(name);
    if (it == counters_.end()) {
        counters_[name] = std::make_unique<Counter>(name);
        it = counters_.find(name);
    }
    return *it->second;
}

Gauge& MetricsRegistry::gauge(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gauges_.find(name);
    if (it == gauges_.end()) {
        gauges_[name] = std::make_unique<Gauge>(name);
        it = gauges_.find(name);
    }
    return *it->second;
}

LatencyHistogram& MetricsRegistry::histogram(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = histograms_.find(name);
    if (it == histograms_.end()) {
        histograms_[name] = std::make_unique<LatencyHistogram>(name);
        it = histograms_.find(name);
    }
    return *it->second;
}

std::string MetricsRegistry::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);

    nlohmann::json j;

    nlohmann::json counters_json;
    for (const auto& [name, counter] : counters_) {
        counters_json[name] = counter->value();
    }
    j["counters"] = counters_json;

    nlohmann::json gauges_json;
    for (const auto& [name, gauge] : gauges_) {
        gauges_json[name] = gauge->value();
    }
    j["gauges"] = gauges_json;

    nlohmann::json histograms_json;
    for (const auto& [name, hist] : histograms_) {
        nlohmann::json h;
        h["count"] = hist->count();
        h["p50_us"] = std::chrono::duration_cast<std::chrono::microseconds>(hist->p50()).count();
        h["p95_us"] = std::chrono::duration_cast<std::chrono::microseconds>(hist->p95()).count();
        h["p99_us"] = std::chrono::duration_cast<std::chrono::microseconds>(hist->p99()).count();
        histograms_json[name] = h;
    }
    j["histograms"] = histograms_json;

    return j.dump(2);
}

void MetricsRegistry::reset_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, counter] : counters_) {
        counter->reset();
    }
    for (auto& [name, gauge] : gauges_) {
        gauge->set(0.0);
    }
    for (auto& [name, hist] : histograms_) {
        hist->reset();
    }
}

// ScopedLatency implementation

ScopedLatency::ScopedLatency(LatencyHistogram& histogram)
    : histogram_(histogram)
    , start_(now())
{
}

ScopedLatency::~ScopedLatency() {
    if (!stopped_) {
        stop();
    }
}

void ScopedLatency::stop() {
    if (!stopped_) {
        histogram_.record(now() - start_);
        stopped_ = true;
    }
}

Duration ScopedLatency::elapsed() const {
    return now() - start_;
}

} // namespace arb
