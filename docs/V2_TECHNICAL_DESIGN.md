# DailyArb v2.0 - Technical Design Document

## 1. Low-Latency Architecture Redesign

### 1.1 Current Architecture Problems

```
v1.0 Hot Path (Market Update → Order Send):
┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  WebSocket  │────▶│   JSON      │────▶│   Order     │────▶│  Strategy   │
│   Receive   │     │   Parse     │     │   Book      │     │  Evaluate   │
│   (~1ms)    │     │  (~500μs)   │     │  (~100μs)   │     │  (~200μs)   │
└─────────────┘     └─────────────┘     └─────────────┘     └─────────────┘
                                                                   │
                                                                   ▼
┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Order     │◀────│   Risk      │◀────│  Execution  │◀────│   Signal    │
│   Send      │     │   Check     │     │   Engine    │     │  Generate   │
│  (~2ms)     │     │  (~50μs)    │     │  (~100μs)   │     │  (~50μs)    │
└─────────────┘     └─────────────┘     └─────────────┘     └─────────────┘

Total: ~4-5ms (with mutex contention can spike to 10ms+)

Problems:
1. nlohmann/json is slow (~500μs per parse)
2. std::mutex causes contention and priority inversion
3. Single-threaded WebSocket receive blocks processing
4. No pipelining - each step waits for previous
```

### 1.2 v2.0 Target Architecture

```
v2.0 Hot Path (Target: <500μs total):

        ┌──────────────────────────────────────────────────────────┐
        │                    Network Thread (pinned)               │
        │  ┌─────────────┐     ┌─────────────┐                     │
        │  │  io_uring   │────▶│   simdjson  │────▶ SPSC Queue ────┼──▶
        │  │   recv      │     │   parse     │     (lock-free)     │
        │  │  (~100μs)   │     │  (~50μs)    │                     │
        │  └─────────────┘     └─────────────┘                     │
        └──────────────────────────────────────────────────────────┘

        ┌──────────────────────────────────────────────────────────┐
        │                   Strategy Thread (pinned)               │
        │                           │                              │
◀───────┼─── SPSC Queue ◀────┬─────┴─────┬──────────────────┐      │
        │                    │           │                  │      │
        │         ┌──────────▼───┐ ┌─────▼─────┐ ┌──────────▼───┐  │
        │         │ Lock-free    │ │ Strategy  │ │    Risk      │  │
        │         │ Order Book   │ │ Evaluate  │ │    Check     │  │
        │         │  (~20μs)     │ │ (~100μs)  │ │   (~20μs)    │  │
        │         └──────────────┘ └───────────┘ └──────────────┘  │
        └──────────────────────────────────────────────────────────┘

        ┌──────────────────────────────────────────────────────────┐
        │                   Execution Thread (pinned)              │
        │  ┌─────────────┐     ┌─────────────┐                     │
◀───────┼──│   Order     │◀────│   Async     │◀─── SPSC Queue ◀───┼───
        │  │   Send      │     │   Batch     │                     │
        │  │  (~200μs)   │     │  (~10μs)    │                     │
        │  └─────────────┘     └─────────────┘                     │
        └──────────────────────────────────────────────────────────┘

Total: ~400-500μs (10x improvement)
```

### 1.3 SPSC Queue Implementation

```cpp
// Cache-line aligned, lock-free single-producer single-consumer queue
template<typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

    struct alignas(64) {
        std::atomic<size_t> head{0};
        char padding1[64 - sizeof(std::atomic<size_t>)];
    };

    struct alignas(64) {
        std::atomic<size_t> tail{0};
        char padding2[64 - sizeof(std::atomic<size_t>)];
    };

    struct alignas(64) {
        std::atomic<size_t> cached_head{0};  // Producer's cache of head
        char padding3[64 - sizeof(std::atomic<size_t>)];
    };

    struct alignas(64) {
        std::atomic<size_t> cached_tail{0};  // Consumer's cache of tail
        char padding4[64 - sizeof(std::atomic<size_t>)];
    };

    alignas(64) std::array<T, Capacity> buffer_;

public:
    bool try_push(const T& item) {
        const size_t t = tail.load(std::memory_order_relaxed);
        const size_t next_t = (t + 1) & MASK;

        if (next_t == cached_head.load(std::memory_order_relaxed)) {
            cached_head.store(head.load(std::memory_order_acquire),
                             std::memory_order_relaxed);
            if (next_t == cached_head.load(std::memory_order_relaxed)) {
                return false;  // Queue full
            }
        }

        buffer_[t] = item;
        tail.store(next_t, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) {
        const size_t h = head.load(std::memory_order_relaxed);

        if (h == cached_tail.load(std::memory_order_relaxed)) {
            cached_tail.store(tail.load(std::memory_order_acquire),
                             std::memory_order_relaxed);
            if (h == cached_tail.load(std::memory_order_relaxed)) {
                return false;  // Queue empty
            }
        }

        item = buffer_[h];
        head.store((h + 1) & MASK, std::memory_order_release);
        return true;
    }
};
```

### 1.4 Lock-Free Order Book with Seqlock

```cpp
class SeqlockOrderBook {
    // Seqlock for readers - allows wait-free reads
    std::atomic<uint64_t> sequence_{0};

    // Best bid/ask - updated atomically
    struct alignas(64) TopOfBook {
        double bid_price{0};
        double bid_size{0};
        double ask_price{0};
        double ask_size{0};
        int64_t timestamp_ns{0};
    } tob_;

    // Full book levels (less frequently accessed)
    struct BookLevel {
        std::atomic<double> price{0};
        std::atomic<double> size{0};
    };
    std::array<BookLevel, 10> bid_levels_;
    std::array<BookLevel, 10> ask_levels_;

public:
    // Writer (single thread only)
    void update_tob(double bid_price, double bid_size,
                    double ask_price, double ask_size) {
        // Increment sequence (odd = write in progress)
        sequence_.fetch_add(1, std::memory_order_release);

        // Update data
        tob_.bid_price = bid_price;
        tob_.bid_size = bid_size;
        tob_.ask_price = ask_price;
        tob_.ask_size = ask_size;
        tob_.timestamp_ns = now_ns();

        // Increment sequence (even = write complete)
        sequence_.fetch_add(1, std::memory_order_release);
    }

    // Reader (multiple threads, wait-free)
    TopOfBook read_tob() const {
        TopOfBook result;
        uint64_t seq1, seq2;

        do {
            seq1 = sequence_.load(std::memory_order_acquire);

            // Read data
            result.bid_price = tob_.bid_price;
            result.bid_size = tob_.bid_size;
            result.ask_price = tob_.ask_price;
            result.ask_size = tob_.ask_size;
            result.timestamp_ns = tob_.timestamp_ns;

            seq2 = sequence_.load(std::memory_order_acquire);

        } while (seq1 != seq2 || (seq1 & 1));  // Retry if write in progress

        return result;
    }

    // Computed values
    double spread() const {
        auto tob = read_tob();
        return tob.ask_price - tob.bid_price;
    }

    double mid_price() const {
        auto tob = read_tob();
        return (tob.bid_price + tob.ask_price) / 2.0;
    }
};
```

---

## 2. Improved Execution Engine

### 2.1 Async Order Management

```cpp
class AsyncExecutionEngine {
    // Order tracking with minimal locking
    struct OrderSlot {
        std::atomic<OrderState> state{OrderState::EMPTY};
        std::atomic<uint64_t> sequence{0};
        Order order;  // Only accessed when state != EMPTY
    };

    // Pre-allocated order slots
    std::array<OrderSlot, 1024> order_slots_;
    std::atomic<size_t> next_slot_{0};

    // Outbound order queue
    SPSCQueue<OrderRequest, 256> outbound_queue_;

    // Inbound fill queue
    SPSCQueue<Fill, 256> inbound_fills_;

public:
    // Non-blocking order submission
    std::optional<OrderId> submit_order(const Signal& signal) {
        // Find empty slot (lock-free)
        size_t slot = find_empty_slot();
        if (slot == INVALID_SLOT) {
            return std::nullopt;  // No slots available
        }

        // Initialize order
        OrderSlot& os = order_slots_[slot];
        os.order = create_order(signal);
        os.sequence.fetch_add(1, std::memory_order_release);
        os.state.store(OrderState::PENDING, std::memory_order_release);

        // Queue for sending
        OrderRequest req{slot, os.order};
        if (!outbound_queue_.try_push(req)) {
            os.state.store(OrderState::EMPTY, std::memory_order_release);
            return std::nullopt;
        }

        return OrderId{slot, os.sequence.load()};
    }

    // Process fills (called from network thread)
    void process_fills() {
        Fill fill;
        while (inbound_fills_.try_pop(fill)) {
            auto slot = find_slot_by_exchange_id(fill.exchange_order_id);
            if (slot != INVALID_SLOT) {
                apply_fill(order_slots_[slot], fill);
            }
        }
    }
};
```

### 2.2 Smart Paired Order Execution

```cpp
class SmartPairExecutor {
public:
    struct PairConfig {
        Duration max_spread_time{std::chrono::milliseconds(100)};
        double max_leg_slippage_bps{10.0};
        bool use_ioc{true};
        bool auto_hedge_partial{true};
    };

    struct PairResult {
        bool both_filled{false};
        double total_cost{0};
        double expected_payout{0};
        double actual_edge{0};
        std::string error;
    };

    PairResult execute_atomic_pair(
        const Signal& yes_signal,
        const Signal& no_signal,
        const PairConfig& config
    ) {
        PairResult result;

        // 1. Pre-flight checks
        if (!validate_signals(yes_signal, no_signal)) {
            result.error = "Invalid signals";
            return result;
        }

        // 2. Snapshot current book state
        auto yes_book = get_book_snapshot(yes_signal.token_id);
        auto no_book = get_book_snapshot(no_signal.token_id);

        // 3. Check if edge still exists
        double current_edge = calculate_edge(yes_book, no_book);
        if (current_edge < min_edge_threshold_) {
            result.error = "Edge disappeared";
            return result;
        }

        // 4. Calculate optimal order sizes
        auto [yes_size, no_size] = calculate_matched_sizes(
            yes_book, no_book, yes_signal.target_size
        );

        // 5. Send both orders simultaneously
        auto yes_future = send_order_async(yes_signal.token_id, Side::BUY,
                                           yes_book.best_ask, yes_size, config.use_ioc);
        auto no_future = send_order_async(no_signal.token_id, Side::BUY,
                                          no_book.best_ask, no_size, config.use_ioc);

        // 6. Wait for both with timeout
        auto yes_result = yes_future.wait_for(config.max_spread_time);
        auto no_result = no_future.wait_for(config.max_spread_time);

        // 7. Handle results
        if (yes_result.filled && no_result.filled) {
            result.both_filled = true;
            result.total_cost = yes_result.fill_price * yes_result.fill_size +
                               no_result.fill_price * no_result.fill_size;
            result.expected_payout = 1.0;
            result.actual_edge = result.expected_payout - result.total_cost;
        }
        else if (yes_result.filled != no_result.filled) {
            // Partial fill - need to hedge
            if (config.auto_hedge_partial) {
                hedge_partial_fill(yes_result, no_result);
            }
            result.error = "Partial fill - hedged";
        }
        else {
            result.error = "Neither side filled";
        }

        return result;
    }

private:
    void hedge_partial_fill(const OrderResult& yes, const OrderResult& no) {
        if (yes.filled && !no.filled) {
            // Sell YES at market to close exposure
            auto hedge = send_market_order(yes.token_id, Side::SELL, yes.fill_size);
            log_hedge("YES", hedge);
        }
        else if (!yes.filled && no.filled) {
            // Sell NO at market
            auto hedge = send_market_order(no.token_id, Side::SELL, no.fill_size);
            log_hedge("NO", hedge);
        }
    }
};
```

---

## 3. Enhanced Risk Management

### 3.1 Real-Time VaR Calculation

```cpp
class RealTimeRiskCalculator {
public:
    struct RiskMetrics {
        double var_95{0};      // 95% Value at Risk (1-day)
        double var_99{0};      // 99% Value at Risk
        double expected_shortfall{0};
        double max_drawdown{0};
        double current_drawdown{0};
        double beta_to_btc{0};
    };

    RiskMetrics calculate() {
        RiskMetrics metrics;

        // Get current positions
        auto positions = position_manager_->get_open_positions();

        // Calculate portfolio value and sensitivities
        double portfolio_value = 0;
        double btc_sensitivity = 0;

        for (const auto& pos : positions) {
            portfolio_value += pos.market_value();
            btc_sensitivity += calculate_btc_sensitivity(pos);
        }

        // Historical simulation VaR
        auto returns = get_historical_returns(252);  // 1 year
        std::sort(returns.begin(), returns.end());

        metrics.var_95 = -portfolio_value * returns[static_cast<size_t>(0.05 * returns.size())];
        metrics.var_99 = -portfolio_value * returns[static_cast<size_t>(0.01 * returns.size())];

        // Expected shortfall (average of worst 5%)
        double es_sum = 0;
        size_t es_count = static_cast<size_t>(0.05 * returns.size());
        for (size_t i = 0; i < es_count; i++) {
            es_sum += returns[i];
        }
        metrics.expected_shortfall = -portfolio_value * (es_sum / es_count);

        // Drawdown
        metrics.max_drawdown = max_drawdown_;
        metrics.current_drawdown = peak_value_ - portfolio_value;

        return metrics;
    }
};
```

### 3.2 Dynamic Position Sizing

```cpp
class DynamicPositionSizer {
public:
    struct SizingConfig {
        double kelly_fraction{0.25};  // Quarter Kelly for safety
        double max_position_pct{0.10};  // Max 10% of capital per position
        double vol_scaling{true};  // Scale size inversely with volatility
    };

    double calculate_optimal_size(
        const Signal& signal,
        double available_capital,
        const SizingConfig& config
    ) {
        // Kelly Criterion: f* = (bp - q) / b
        // where b = odds, p = prob of win, q = prob of loss

        double win_prob = estimate_win_probability(signal);
        double win_amount = signal.expected_edge / 100.0;  // Convert cents to dollars
        double loss_amount = signal.target_price;  // Lose the cost if wrong

        double b = win_amount / loss_amount;
        double p = win_prob;
        double q = 1.0 - p;

        double kelly_fraction = (b * p - q) / b;

        // Apply fractional Kelly
        kelly_fraction *= config.kelly_fraction;

        // Clamp to max position
        kelly_fraction = std::min(kelly_fraction, config.max_position_pct);

        // Volatility adjustment
        if (config.vol_scaling) {
            double vol = realized_vol_tracker_.current();
            double vol_baseline = 0.02;  // 2% daily vol baseline
            double vol_scalar = vol_baseline / std::max(vol, 0.001);
            kelly_fraction *= std::clamp(vol_scalar, 0.5, 2.0);
        }

        double size = available_capital * kelly_fraction / signal.target_price;

        return std::max(0.0, size);
    }

private:
    double estimate_win_probability(const Signal& signal) {
        // Use historical win rate for this strategy
        auto stats = strategy_stats_.get(signal.strategy_name);
        if (stats.total_trades > 30) {
            return stats.win_rate;
        }
        // Default conservative estimate
        return 0.55;
    }
};
```

---

## 4. New Data Sources

### 4.1 Binance Order Book Depth

```cpp
class BinanceDepthClient {
public:
    struct DepthUpdate {
        int64_t last_update_id;
        std::vector<PriceLevel> bids;
        std::vector<PriceLevel> asks;
        int64_t event_time_ms;
    };

    void subscribe_depth(const std::string& symbol, int levels = 20) {
        // Subscribe to depth stream: btcusdt@depth20@100ms
        std::string stream = symbol + "@depth" + std::to_string(levels) + "@100ms";
        subscribe(stream);
    }

    // Calculate market impact
    double estimate_market_impact(Side side, double size) {
        auto book = current_book();
        double remaining = size;
        double total_cost = 0;

        const auto& levels = (side == Side::BUY) ? book.asks : book.bids;

        for (const auto& level : levels) {
            double fill_size = std::min(remaining, level.size);
            total_cost += fill_size * level.price;
            remaining -= fill_size;
            if (remaining <= 0) break;
        }

        double avg_price = total_cost / size;
        double mid = book.mid_price();

        return (avg_price - mid) / mid * 10000;  // Impact in bps
    }
};
```

### 4.2 Polymarket Trade Feed

```cpp
class PolymarketTradeFeed {
public:
    struct Trade {
        std::string market_id;
        std::string token_id;
        Side taker_side;
        double price;
        double size;
        int64_t timestamp_ms;
    };

    // Subscribe to trade feed for flow analysis
    void subscribe_trades(const std::string& token_id) {
        // Polymarket uses "last_trade_price" events
        nlohmann::json sub = {
            {"type", "subscribe"},
            {"channel", "market"},
            {"assets_ids", {token_id}}
        };
        send(sub.dump());
    }

    // Analyze order flow
    struct FlowAnalysis {
        double buy_volume_1m{0};
        double sell_volume_1m{0};
        double vwap_1m{0};
        int trade_count{0};
        double largest_trade{0};
    };

    FlowAnalysis analyze_recent_flow(Duration window = std::chrono::minutes(1)) {
        FlowAnalysis analysis;
        auto cutoff = now() - window;

        for (const auto& trade : recent_trades_) {
            if (trade.timestamp < cutoff) continue;

            if (trade.taker_side == Side::BUY) {
                analysis.buy_volume_1m += trade.size;
            } else {
                analysis.sell_volume_1m += trade.size;
            }
            analysis.trade_count++;
            analysis.largest_trade = std::max(analysis.largest_trade, trade.size);
        }

        return analysis;
    }
};
```

---

## 5. Testing Infrastructure

### 5.1 Deterministic Replay Testing

```cpp
class ReplayTestFramework {
public:
    struct ReplayConfig {
        std::string data_file;
        double speed_multiplier{1.0};  // 1.0 = real-time, 0 = as fast as possible
        bool deterministic{true};
    };

    // Load recorded market data
    void load_data(const std::string& path) {
        std::ifstream file(path);
        std::string line;

        while (std::getline(file, line)) {
            auto event = parse_event(line);
            events_.push_back(event);
        }

        std::sort(events_.begin(), events_.end(),
                  [](const auto& a, const auto& b) {
                      return a.timestamp < b.timestamp;
                  });
    }

    // Run replay
    ReplayResult run(StrategyBase& strategy, const ReplayConfig& config) {
        ReplayResult result;

        // Create simulated market data feeds
        SimulatedBinanceClient binance;
        SimulatedPolymarketClient polymarket;

        // Create execution simulator
        ExecutionSimulator exec_sim;

        int64_t last_time = 0;

        for (const auto& event : events_) {
            // Wait for appropriate time if real-time replay
            if (config.speed_multiplier > 0 && last_time > 0) {
                int64_t delay_ns = (event.timestamp - last_time) / config.speed_multiplier;
                precise_sleep(std::chrono::nanoseconds(delay_ns));
            }
            last_time = event.timestamp;

            // Inject event
            if (event.type == EventType::BINANCE_TICKER) {
                binance.inject(event.data);
            } else if (event.type == EventType::POLYMARKET_BOOK) {
                polymarket.inject(event.data);
            }

            // Evaluate strategy
            auto signals = strategy.evaluate(
                polymarket.get_book(),
                binance.get_price(),
                Timestamp(std::chrono::nanoseconds(event.timestamp))
            );

            // Simulate execution
            for (const auto& signal : signals) {
                auto fill = exec_sim.simulate_fill(signal, polymarket.get_book());
                result.trades.push_back(fill);
                result.total_pnl += fill.pnl;
            }
        }

        result.calculate_statistics();
        return result;
    }
};
```

### 5.2 Property-Based Testing

```cpp
class PropertyBasedTests {
public:
    // Test that order book invariants always hold
    void test_order_book_invariants() {
        rapid_check::check([](std::vector<std::pair<double, double>> updates) {
            OrderBook book("TEST");

            for (const auto& [price, size] : updates) {
                if (price > 0.5) {
                    book.update_ask(price, std::abs(size));
                } else {
                    book.update_bid(price, std::abs(size));
                }
            }

            // Invariant: best_bid < best_ask (if both exist)
            auto bid = book.best_bid();
            auto ask = book.best_ask();
            if (bid && ask) {
                REQUIRE(bid->price < ask->price);
            }

            // Invariant: all bids sorted descending
            auto bids = book.top_bids(100);
            for (size_t i = 1; i < bids.size(); i++) {
                REQUIRE(bids[i-1].price >= bids[i].price);
            }

            return true;
        });
    }

    // Test risk manager constraints
    void test_risk_manager_constraints() {
        rapid_check::check([](
            double starting_balance,
            std::vector<double> trade_pnls
        ) {
            if (starting_balance <= 0) return true;

            RiskConfig config;
            config.max_daily_loss = starting_balance * 0.1;
            RiskManager rm(config, starting_balance);

            double total_pnl = 0;
            for (double pnl : trade_pnls) {
                rm.record_pnl(pnl);
                total_pnl += pnl;

                // Invariant: daily_pnl tracks correctly
                REQUIRE(std::abs(rm.daily_pnl() - total_pnl) < 0.001);

                // Invariant: kill switch triggers on excessive loss
                if (total_pnl < -config.max_daily_loss) {
                    REQUIRE(rm.is_kill_switch_active() ||
                           !rm.check_daily_loss().allowed);
                }
            }

            return true;
        });
    }
};
```

---

## 6. Deployment & Operations

### 6.1 Container Configuration

```dockerfile
# Dockerfile for v2
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential cmake ninja-build \
    libssl-dev libcurl4-openssl-dev libncurses-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN mkdir build && cd build && \
    cmake -GNinja -DCMAKE_BUILD_TYPE=Release .. && \
    ninja

# Runtime image
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libssl3 libcurl4 libncurses6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /app/build/dailyarb /usr/local/bin/
COPY --from=builder /app/configs /etc/dailyarb/

# Non-root user for security
RUN useradd -m -s /bin/bash arb
USER arb

EXPOSE 9090

ENTRYPOINT ["/usr/local/bin/dailyarb"]
CMD ["--config", "/etc/dailyarb/bot.json", "--dry-run"]
```

### 6.2 Kubernetes Deployment

```yaml
# k8s/deployment.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: dailyarb
spec:
  replicas: 1  # Single instance to avoid duplicate orders
  selector:
    matchLabels:
      app: dailyarb
  template:
    metadata:
      labels:
        app: dailyarb
    spec:
      containers:
      - name: dailyarb
        image: dailyarb:v2.0
        args:
          - "--config"
          - "/etc/dailyarb/bot.json"
          - "--paper"
        resources:
          requests:
            cpu: "2"
            memory: "512Mi"
          limits:
            cpu: "4"
            memory: "1Gi"
        env:
        - name: POLYMARKET_API_KEY
          valueFrom:
            secretKeyRef:
              name: polymarket-credentials
              key: api-key
        - name: POLYMARKET_API_SECRET
          valueFrom:
            secretKeyRef:
              name: polymarket-credentials
              key: api-secret
        ports:
        - containerPort: 9090
          name: metrics
        livenessProbe:
          httpGet:
            path: /health
            port: 9090
          initialDelaySeconds: 30
          periodSeconds: 10
        readinessProbe:
          httpGet:
            path: /ready
            port: 9090
          initialDelaySeconds: 5
          periodSeconds: 5
        volumeMounts:
        - name: config
          mountPath: /etc/dailyarb
        - name: data
          mountPath: /var/lib/dailyarb
      volumes:
      - name: config
        configMap:
          name: dailyarb-config
      - name: data
        persistentVolumeClaim:
          claimName: dailyarb-data
```

### 6.3 Monitoring Stack

```yaml
# k8s/monitoring.yaml
apiVersion: monitoring.coreos.com/v1
kind: ServiceMonitor
metadata:
  name: dailyarb
spec:
  selector:
    matchLabels:
      app: dailyarb
  endpoints:
  - port: metrics
    interval: 10s
---
apiVersion: monitoring.coreos.com/v1
kind: PrometheusRule
metadata:
  name: dailyarb-alerts
spec:
  groups:
  - name: dailyarb
    rules:
    - alert: HighLatency
      expr: histogram_quantile(0.99, dailyarb_latency_seconds) > 0.01
      for: 5m
      labels:
        severity: warning
      annotations:
        summary: "High order latency detected"
    - alert: ConnectionLost
      expr: dailyarb_connection_status == 0
      for: 1m
      labels:
        severity: critical
      annotations:
        summary: "Exchange connection lost"
    - alert: DailyLossExceeded
      expr: dailyarb_daily_pnl < -5
      labels:
        severity: critical
      annotations:
        summary: "Daily loss limit exceeded"
```

---

## Summary

This technical design document covers the major architectural changes and new features planned for v2.0. The key improvements are:

1. **10x latency reduction** through lock-free data structures and simdjson
2. **Better execution** with async order management and smart paired execution
3. **Enhanced risk management** with real-time VaR and dynamic sizing
4. **More data sources** for better signal generation
5. **Comprehensive testing** infrastructure
6. **Production-ready deployment** configuration

Total estimated development effort: 12-16 weeks for full implementation.
