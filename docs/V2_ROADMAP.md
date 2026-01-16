# DailyArb v2.0 - Development Roadmap

## Executive Summary

This document outlines the planned improvements and new features for DailyArb v2.0, organized by priority and effort level. The focus areas are:

1. **Latency Optimization** - Reduce decision-to-order time from ~10ms to <1ms
2. **Execution Quality** - Improve fill rates and reduce slippage
3. **Strategy Expansion** - Add more sophisticated signal generation
4. **Production Hardening** - Make the system robust for 24/7 operation
5. **Analytics & ML** - Data-driven improvements and adaptive behavior

---

## Priority 1: Critical Improvements (High Impact, Medium Effort)

### 1.1 Lock-Free Architecture

**Current State**: Uses `std::mutex` for thread synchronization
**Target**: Lock-free data structures for hot paths

```cpp
// v2 Implementation Plan
namespace arb::v2 {

// Single-Producer Single-Consumer queue for market data
template<typename T, size_t Capacity>
class SPSCQueue {
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::array<T, Capacity> buffer_;
public:
    bool try_push(const T& item);
    bool try_pop(T& item);
    bool empty() const;
};

// Lock-free order book with seqlock for readers
class LockFreeOrderBook {
    std::atomic<uint64_t> sequence_{0};
    alignas(64) PriceLevel best_bid_;
    alignas(64) PriceLevel best_ask_;
public:
    void update(const PriceLevel& bid, const PriceLevel& ask);
    std::pair<PriceLevel, PriceLevel> read() const; // Seqlock read
};

}
```

**Estimated Latency Improvement**: 2-5x for hot path operations

---

### 1.2 Simdjson Integration

**Current State**: Uses nlohmann/json (convenient but slow)
**Target**: Use simdjson for parsing market data

```cpp
// v2 JSON Parsing
#include <simdjson.h>

class FastMessageParser {
    simdjson::ondemand::parser parser_;
    simdjson::padded_string buffer_;

public:
    // Parse Binance bookTicker in <100ns
    BtcPrice parse_binance_ticker(std::string_view json) {
        auto doc = parser_.iterate(json);
        BtcPrice price;
        price.bid = doc["b"].get_double();
        price.ask = doc["a"].get_double();
        price.exchange_time_ms = doc["E"].get_int64();
        return price;
    }

    // Parse Polymarket book message
    void parse_polymarket_book(std::string_view json, OrderBook& book);
};
```

**Estimated Improvement**: 10-50x faster JSON parsing

---

### 1.3 Connection Multiplexing & Failover

**Current State**: Single connection per venue, basic reconnect
**Target**: Multiple redundant connections with automatic failover

```cpp
// v2 Connection Manager
class ConnectionPool {
public:
    struct Config {
        std::vector<std::string> endpoints;  // Multiple endpoints
        int connections_per_endpoint{2};
        int failover_timeout_ms{100};
    };

    // Automatic failover on disconnect
    void send(const std::string& msg) {
        for (auto& conn : active_connections_) {
            if (conn->is_healthy()) {
                conn->send(msg);
                return;
            }
        }
        // All connections down - trigger kill switch
    }

private:
    std::vector<std::unique_ptr<Connection>> connections_;
    std::atomic<Connection*> primary_{nullptr};
};
```

---

### 1.4 Improved Paired Order Execution

**Current State**: Sequential IOC orders with manual unwind
**Target**: Atomic-style execution with automatic hedging

```cpp
// v2 Paired Execution Engine
class AtomicPairExecutor {
public:
    struct PairConfig {
        Duration max_leg_delay{std::chrono::milliseconds(50)};
        double max_partial_exposure{0.5};  // Max exposure if only one fills
        bool auto_hedge{true};
    };

    // Execute both legs with smart timing
    PairResult execute_pair(
        const Signal& yes_signal,
        const Signal& no_signal,
        const PairConfig& config
    ) {
        // 1. Send both orders simultaneously
        auto yes_future = async_send(yes_signal);
        auto no_future = async_send(no_signal);

        // 2. Wait for both with timeout
        auto [yes_result, no_result] = wait_both(
            yes_future, no_future, config.max_leg_delay
        );

        // 3. Handle partial fills
        if (yes_result.filled && !no_result.filled) {
            if (config.auto_hedge) {
                return hedge_position(yes_result, Side::SELL);
            }
        }

        return {yes_result, no_result};
    }
};
```

---

## Priority 2: New Strategies (High Impact, High Effort)

### 2.1 Strategy S4: Cross-Market Arbitrage

Detect price discrepancies between correlated markets.

```cpp
// Correlate BTC 15m with BTC 1h markets
class CrossMarketStrategy : public StrategyBase {
public:
    struct MarketPair {
        std::string short_term_market;  // BTC 15m
        std::string long_term_market;   // BTC 1h
        double correlation_threshold{0.8};
    };

    std::vector<Signal> evaluate(
        const std::map<std::string, BinaryMarketBook>& books,
        const BtcPrice& btc_price,
        Timestamp now
    ) override {
        // If 15m YES is at 0.70 and 1h YES is at 0.40
        // and BTC has been rising, the 1h market may be lagging

        double short_prob = get_implied_prob(books, short_term_market_);
        double long_prob = get_implied_prob(books, long_term_market_);

        // Detect divergence
        if (short_prob - long_prob > divergence_threshold_) {
            // Generate signal to buy long-term YES
        }
    }
};
```

### 2.2 Strategy S5: Volatility-Adjusted Probability

Use BTC volatility to improve probability estimates.

```cpp
class VolatilityStrategy : public StrategyBase {
    // Track realized volatility
    RollingVolatility vol_tracker_{60};  // 60-second window

public:
    double calculate_fair_probability(
        double current_btc_price,
        double strike_price,  // BTC target for "up" outcome
        Duration time_to_expiry,
        double volatility
    ) {
        // Black-Scholes inspired probability calculation
        // P(BTC > strike) = N(d2) where d2 = (ln(S/K) + (r - σ²/2)T) / (σ√T)

        double t = duration_to_years(time_to_expiry);
        double d2 = (std::log(current_btc_price / strike_price) -
                    0.5 * volatility * volatility * t) /
                    (volatility * std::sqrt(t));

        return normal_cdf(d2);
    }
};
```

### 2.3 Strategy S6: Order Flow Imbalance

Detect aggressive order flow to predict short-term price moves.

```cpp
class OrderFlowStrategy : public StrategyBase {
    struct FlowMetrics {
        double buy_volume_1s{0};
        double sell_volume_1s{0};
        double imbalance{0};  // (buy - sell) / (buy + sell)
        int aggressive_buy_count{0};
        int aggressive_sell_count{0};
    };

    FlowMetrics calculate_flow(const std::vector<Trade>& trades) {
        FlowMetrics metrics;

        for (const auto& trade : trades) {
            if (trade.is_buyer_maker) {
                // Sell aggressor - hitting bid
                metrics.sell_volume_1s += trade.size;
                metrics.aggressive_sell_count++;
            } else {
                // Buy aggressor - lifting offer
                metrics.buy_volume_1s += trade.size;
                metrics.aggressive_buy_count++;
            }
        }

        double total = metrics.buy_volume_1s + metrics.sell_volume_1s;
        if (total > 0) {
            metrics.imbalance = (metrics.buy_volume_1s - metrics.sell_volume_1s) / total;
        }

        return metrics;
    }
};
```

---

## Priority 3: Production Hardening (Medium Impact, Medium Effort)

### 3.1 Graceful Degradation

```cpp
class DegradationManager {
public:
    enum class Mode {
        FULL,           // All strategies active
        REDUCED,        // Only S2 (safest strategy)
        HEDGE_ONLY,     // Only close positions
        HALTED          // No trading
    };

    Mode evaluate_system_health() {
        auto binance_health = binance_client_->health_score();
        auto poly_health = polymarket_client_->health_score();
        auto latency = get_p99_latency();

        if (binance_health < 0.5 || poly_health < 0.5) {
            return Mode::HALTED;
        }
        if (latency > max_acceptable_latency_) {
            return Mode::REDUCED;
        }
        if (risk_manager_->near_limits()) {
            return Mode::HEDGE_ONLY;
        }
        return Mode::FULL;
    }
};
```

### 3.2 State Persistence & Recovery

```cpp
class StateManager {
public:
    // Checkpoint state every N seconds
    void checkpoint() {
        Checkpoint cp;
        cp.timestamp = wall_now();
        cp.positions = position_manager_->snapshot();
        cp.open_orders = execution_engine_->get_open_orders();
        cp.daily_pnl = risk_manager_->daily_pnl();
        cp.strategy_state = serialize_strategies();

        write_atomic(checkpoint_path_, cp);
    }

    // Recover on startup
    void recover() {
        auto cp = load_checkpoint();
        if (!cp.valid || is_stale(cp.timestamp)) {
            log_warn("Stale checkpoint, starting fresh");
            return;
        }

        // Reconcile with exchange state
        auto exchange_orders = polymarket_client_->get_open_orders();
        reconcile(cp.open_orders, exchange_orders);

        position_manager_->restore(cp.positions);
        // ...
    }
};
```

### 3.3 Circuit Breakers

```cpp
class CircuitBreaker {
public:
    struct Config {
        int failure_threshold{5};       // Failures before open
        Duration open_duration{30s};    // Time before half-open
        int success_threshold{3};       // Successes to close
    };

    enum class State { CLOSED, OPEN, HALF_OPEN };

    bool allow_request() {
        switch (state_) {
            case State::CLOSED:
                return true;
            case State::OPEN:
                if (now() - last_failure_ > config_.open_duration) {
                    state_ = State::HALF_OPEN;
                    return true;
                }
                return false;
            case State::HALF_OPEN:
                return true;
        }
    }

    void record_success() {
        if (state_ == State::HALF_OPEN) {
            if (++success_count_ >= config_.success_threshold) {
                state_ = State::CLOSED;
                failure_count_ = 0;
            }
        }
    }

    void record_failure() {
        last_failure_ = now();
        if (++failure_count_ >= config_.failure_threshold) {
            state_ = State::OPEN;
        }
    }
};
```

---

## Priority 4: Analytics & Observability (Medium Impact, Low Effort)

### 4.1 Prometheus/Grafana Integration

```cpp
// Expose metrics endpoint
class MetricsExporter {
public:
    void expose_http(int port = 9090) {
        http_server_.route("/metrics", [this](auto& req, auto& res) {
            std::string output;

            // Counters
            output += format_counter("dailyarb_orders_total",
                                    orders_submitted_.load());
            output += format_counter("dailyarb_fills_total",
                                    fills_received_.load());

            // Gauges
            output += format_gauge("dailyarb_pnl_dollars",
                                  position_manager_->total_pnl());
            output += format_gauge("dailyarb_exposure_dollars",
                                  risk_manager_->current_exposure());

            // Histograms
            output += format_histogram("dailyarb_latency_seconds",
                                      latency_histogram_);

            res.set_content(output, "text/plain");
        });
    }
};
```

### 4.2 Trade Analytics Dashboard

```cpp
struct TradeAnalytics {
    // Per-strategy metrics
    struct StrategyMetrics {
        int signals_generated{0};
        int signals_traded{0};
        int winning_trades{0};
        int losing_trades{0};
        double total_pnl{0};
        double sharpe_ratio{0};
        double max_drawdown{0};
        double avg_edge_captured{0};  // vs expected edge
    };

    std::map<std::string, StrategyMetrics> by_strategy;

    // Time-based analysis
    struct HourlyMetrics {
        int hour;
        double pnl;
        int trades;
        double avg_spread;
    };
    std::array<HourlyMetrics, 24> by_hour;

    // Generate reports
    std::string generate_daily_report();
    std::string generate_strategy_comparison();
};
```

### 4.3 Alerting System

```cpp
class AlertManager {
public:
    enum class Severity { INFO, WARNING, CRITICAL };

    struct Alert {
        Severity severity;
        std::string message;
        WallClock timestamp;
        std::map<std::string, std::string> labels;
    };

    void check_alerts() {
        // PnL alerts
        if (daily_pnl < -loss_threshold_) {
            fire(Severity::CRITICAL, "Daily loss threshold exceeded",
                 {{"pnl", std::to_string(daily_pnl)}});
        }

        // Latency alerts
        if (p99_latency > latency_threshold_) {
            fire(Severity::WARNING, "High latency detected",
                 {{"p99_ms", std::to_string(p99_latency.count())}});
        }

        // Connection alerts
        if (!binance_->is_connected() || !polymarket_->is_connected()) {
            fire(Severity::CRITICAL, "Connection lost");
        }
    }

    // Notification channels
    void send_slack(const Alert& alert);
    void send_telegram(const Alert& alert);
    void send_email(const Alert& alert);
};
```

---

## Priority 5: Advanced Features (Lower Priority, High Effort)

### 5.1 Machine Learning Signal Enhancement

```cpp
// ONNX Runtime for ML inference
class MLSignalEnhancer {
    Ort::Session session_;

public:
    MLSignalEnhancer(const std::string& model_path) {
        Ort::Env env;
        session_ = Ort::Session(env, model_path.c_str(), Ort::SessionOptions{});
    }

    // Enhance signal confidence using ML
    double enhance_confidence(const Signal& signal, const Features& features) {
        // Features: spread, depth, volatility, time_to_expiry, flow_imbalance
        std::vector<float> input = features.to_vector();

        auto output = session_.Run(
            Ort::RunOptions{nullptr},
            input_names_.data(), &input_tensor_, 1,
            output_names_.data(), 1
        );

        return output[0].GetTensorData<float>()[0];
    }
};

// Feature engineering
struct Features {
    double spread_bps;
    double depth_imbalance;  // (bid_depth - ask_depth) / total
    double realized_vol_1m;
    double btc_momentum_5m;
    double time_to_expiry_mins;
    double order_flow_imbalance;
    double market_cap_rank;  // Size of this market vs others

    std::vector<float> to_vector() const;
};
```

### 5.2 Adaptive Parameter Tuning

```cpp
class AdaptiveParameterTuner {
public:
    // Parameters to tune
    struct Parameters {
        double min_edge_cents;
        double lag_threshold_bps;
        int staleness_window_ms;
        double confidence_threshold;
    };

    // Online learning of optimal parameters
    void update_from_trade(const Trade& trade, const Signal& signal) {
        // Track which parameter combinations lead to profitable trades
        ParameterRegion region = get_region(current_params_);
        region.record_outcome(trade.pnl > 0);

        // Thompson sampling for exploration/exploitation
        if (should_explore()) {
            current_params_ = sample_new_params();
        } else {
            current_params_ = best_known_params();
        }
    }

private:
    std::map<ParameterRegion, BetaDistribution> param_posteriors_;
};
```

### 5.3 Multi-Asset Expansion

```cpp
// Extend beyond BTC to ETH, SOL, etc.
class MultiAssetArbitrage {
    struct AssetConfig {
        std::string symbol;
        std::string binance_symbol;
        std::regex market_pattern;
        double correlation_to_btc;
    };

    std::vector<AssetConfig> assets_ = {
        {"BTC", "btcusdt", std::regex("btc.*15m"), 1.0},
        {"ETH", "ethusdt", std::regex("eth.*15m"), 0.85},
        {"SOL", "solusdt", std::regex("sol.*15m"), 0.75},
    };

    // Cross-asset correlation arbitrage
    void detect_cross_asset_opportunities() {
        // If BTC moves and ETH hasn't, ETH markets may be stale
        double btc_move = btc_tracker_.recent_move_bps();
        double eth_move = eth_tracker_.recent_move_bps();

        double expected_eth_move = btc_move * correlation_btc_eth_;
        double actual_eth_move = eth_move;

        if (std::abs(expected_eth_move - actual_eth_move) > threshold_) {
            // Signal opportunity
        }
    }
};
```

---

## Implementation Timeline

### Phase 1: Q1 (Weeks 1-4) - Performance Foundation
- [ ] Lock-free order book implementation
- [ ] Simdjson integration
- [ ] Connection pool with failover
- [ ] Basic Prometheus metrics

### Phase 2: Q2 (Weeks 5-8) - Execution Quality
- [ ] Improved paired execution
- [ ] Circuit breakers
- [ ] State persistence & recovery
- [ ] Alerting system

### Phase 3: Q3 (Weeks 9-12) - Strategy Expansion
- [ ] Cross-market arbitrage (S4)
- [ ] Volatility-adjusted probability (S5)
- [ ] Order flow strategy (S6)
- [ ] Strategy backtesting framework

### Phase 4: Q4 (Weeks 13-16) - Advanced Features
- [ ] ML signal enhancement
- [ ] Adaptive parameter tuning
- [ ] Multi-asset expansion
- [ ] Full analytics dashboard

---

## Technical Debt to Address

1. **WebSocket Implementation**: Replace hand-rolled WS with Boost.Beast or libwebsockets
2. **Memory Management**: Add custom allocators for Order/Fill objects
3. **Error Handling**: More comprehensive exception handling and error codes
4. **Testing**: Add integration tests, property-based tests, chaos testing
5. **Documentation**: API documentation, operational runbooks

---

## Resource Requirements

| Phase | Estimated Effort | Key Skills Needed |
|-------|------------------|-------------------|
| Phase 1 | 4 weeks | C++ concurrency, low-latency systems |
| Phase 2 | 4 weeks | Distributed systems, monitoring |
| Phase 3 | 4 weeks | Quantitative finance, statistics |
| Phase 4 | 4 weeks | Machine learning, data engineering |

---

## Success Metrics for v2.0

| Metric | v1.0 Baseline | v2.0 Target |
|--------|---------------|-------------|
| Decision-to-order latency (p99) | ~10ms | <1ms |
| Fill rate on paired orders | ~80% | >95% |
| Uptime | Manual restart | 99.9% (8.7h downtime/year) |
| Sharpe ratio | Unknown | >2.0 |
| Daily signal count | ~10-50 | ~100-500 |
| Strategies | 2 | 5+ |
