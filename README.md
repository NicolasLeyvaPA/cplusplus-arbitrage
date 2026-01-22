# DailyArb - Low-Latency Binary Outcome Arbitrage Bot

A C++ arbitrage bot for Polymarket binary outcome markets, designed for low-latency detection and execution of pricing inefficiencies.

## Overview

DailyArb implements two primary arbitrage strategies:

1. **S1 - Stale-Odds/Lag Arbitrage**: Detects when Polymarket odds are stale relative to real-time BTC price movements from Binance.

2. **S2 - Two-Outcome Underpricing**: Identifies when the sum of best ask prices for YES + NO outcomes is less than 1.0 minus fees, enabling near-riskless profit.

## Features

- Real-time WebSocket connections to Binance (BTC price) and Polymarket (order books)
- Low-latency order book management and signal generation
- Three trading modes: dry-run, paper, and live
- Comprehensive risk management with kill switch
- Terminal UI with real-time updates
- Structured JSON logging and trade ledger
- Latency metrics and performance monitoring
- Replay/backtest capability

## Risk & Reality Check

⚠️ **This is NOT financial advice. Trading involves substantial risk of loss.**

Key considerations:
- **Fees**: Polymarket charges ~2% on winnings
- **Slippage**: Fast-moving markets may have execution slippage
- **Min sizes**: Check minimum order sizes before trading
- **Fills**: IOC/FOK orders may not fill completely
- **Network**: Latency and disconnections can cause missed opportunities
- **Execution risk**: For S2 paired orders, one side may fill while the other doesn't

Starting capital: **$50 USDC** (configurable)

## Building

### Prerequisites

- C++20 compatible compiler (GCC 10+, Clang 12+)
- CMake 3.16+
- OpenSSL development libraries
- libcurl development libraries
- ncurses (optional, for terminal UI)

### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev libcurl4-openssl-dev libncurses5-dev
```

### Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Build with debug symbols

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

## Usage

### Dry-Run Mode (Default)

Compute signals without placing any orders:

```bash
./dailyarb --config ../configs/bot.json --dry-run
```

### Paper Trading Mode

Simulated execution against local order book model:

```bash
./dailyarb --config ../configs/bot.json --paper
```

### Live Trading Mode

Real orders on Polymarket. **USE WITH EXTREME CAUTION.**

```bash
# Set environment variables for API credentials
export POLYMARKET_API_KEY="your-api-key"
export POLYMARKET_API_SECRET="your-api-secret"
export POLYMARKET_API_PASSPHRASE="your-passphrase"

./dailyarb --config ../configs/bot.json --live
```

## Configuration

Edit `configs/bot.json` to customize:

```json
{
  "mode": "dry-run",
  "starting_balance_usdc": 50.0,

  "risk": {
    "max_notional_per_trade": 1.50,
    "max_daily_loss": 5.0,
    "max_open_positions": 3
  },

  "strategy": {
    "min_edge_cents": 2.0,
    "lag_move_threshold_bps": 25.0,
    "enable_s1": true,
    "enable_s2": true
  }
}
```

### Environment Variables

| Variable | Description |
|----------|-------------|
| `POLYMARKET_API_KEY` | Polymarket CLOB API key |
| `POLYMARKET_API_SECRET` | API secret (base64) |
| `POLYMARKET_API_PASSPHRASE` | API passphrase |

## Terminal UI

The terminal displays:
- Connection status (Binance, Polymarket)
- Current BTC price
- Active market order book with YES/NO levels
- Sum of asks and detected edge
- Risk status (PnL, exposure, positions)
- Activity log (signals, trades, errors)

### Keyboard Commands

| Key | Action |
|-----|--------|
| `q` | Quit |
| `k` | Toggle kill switch |
| `c` | Cancel all orders |

## Strategies

### S1: Stale-Odds/Lag Arbitrage

Detects when Polymarket market odds haven't caught up to BTC price movements:

1. Monitor BTC price via Binance WebSocket
2. Track Polymarket "BTC Up/Down 15m" market order books
3. Detect significant BTC moves (> 25 bps default)
4. Check if market book is stale (no updates > 500ms)
5. If implied probability differs from expected, generate signal

### S2: Two-Outcome Underpricing

Exploits pricing inefficiency when sum of asks < 1.0 - fees:

1. For binary market with YES and NO outcomes
2. Check: `best_ask_yes + best_ask_no < 0.98` (after 2% fee)
3. If true, buy both sides simultaneously
4. Guaranteed $1 payout minus fees
5. Profit = payout - cost - fees

**Execution Risk**: If only one side fills:
- Immediately attempt to sell filled side at market
- Or cancel unfilled side and hedge
- Position limits prevent excessive naked exposure

## Risk Management

### Hard Constraints

- **Max notional per trade**: Default $1.50 (3% of $50)
- **Max daily loss**: Default $5.00 (10% of $50)
- **Max open positions**: Default 3
- **Max exposure per market**: Default $3.00
- **Rate limit**: Default 10 orders/minute

### Kill Switch

Automatically activates when:
- Stop loss threshold exceeded (default 10% of starting balance)
- Excessive slippage detected (> 50bps repeatedly)
- Multiple connectivity issues

Manual activation via `k` key in UI.

## Testing

### Run Unit Tests

```bash
cd build
./tests
```

### Replay/Backtest

```bash
./replay_tool --input data/recorded_feed.json --strategy s2 -v
```

## Sanity Checklist

Before running in live mode, verify:

- [ ] Minimum order sizes on Polymarket (check API docs)
- [ ] Current fee structure (currently ~2% on winnings)
- [ ] Both YES + NO outcomes truly sum to $1 at settlement
- [ ] Timestamps are synchronized (NTP)
- [ ] Order book is not stale/desynchronized
- [ ] API credentials are valid and have required permissions
- [ ] Kill switch is functioning
- [ ] Rate limits are not exceeded

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                           Main Event Loop                           │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────────────────┐ │
│  │   Binance   │    │ Polymarket  │    │     Signal Engine       │ │
│  │   Client    │    │   Client    │    │                         │ │
│  │             │    │             │    │  ┌────────┐ ┌────────┐  │ │
│  │ BTC Price   │───▶│ Order Books │───▶│  │  S1   │ │  S2   │  │ │
│  │ (WebSocket) │    │ (WebSocket) │    │  │ Stale │ │Under- │  │ │
│  └─────────────┘    └─────────────┘    │  │ Odds  │ │price  │  │ │
│                                         │  └───┬───┘ └───┬───┘  │ │
│                                         │      │         │      │ │
│                                         └──────┼─────────┼──────┘ │
│                                                │         │        │
│                                                ▼         ▼        │
│                           ┌─────────────────────────────────────┐ │
│                           │          Risk Manager               │ │
│                           │  • Position limits                  │ │
│                           │  • Daily loss limit                 │ │
│                           │  • Kill switch                      │ │
│                           └────────────────┬────────────────────┘ │
│                                            │                      │
│                                            ▼                      │
│                           ┌─────────────────────────────────────┐ │
│                           │        Execution Engine             │ │
│                           │  • Order state machine              │ │
│                           │  • Paper/Live modes                 │ │
│                           │  • Paired order handling            │ │
│                           └────────────────┬────────────────────┘ │
│                                            │                      │
│                                            ▼                      │
│                           ┌─────────────────────────────────────┐ │
│                           │        Position Manager             │ │
│                           │  • PnL tracking                     │ │
│                           │  • Mark-to-market                   │ │
│                           │  • Settlement handling              │ │
│                           └─────────────────────────────────────┘ │
│                                                                   │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────────────┐   │
│  │  Terminal   │    │   Trade     │    │      Metrics        │   │
│  │     UI      │    │   Ledger    │    │     Registry        │   │
│  └─────────────┘    └─────────────┘    └─────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## Optional Latency Upgrades

For further performance optimization:

1. **Thread Pinning**: Pin critical threads to specific CPU cores
2. **Lock-free Queues**: Use SPSC queues for message passing
3. **Faster JSON Parsing**: Use simdjson for message parsing
4. **Kernel Bypass**: Use DPDK or io_uring for network I/O
5. **Memory Pre-allocation**: Pool allocators for order objects
6. **Co-location**: Deploy near exchange infrastructure
7. **Binary Protocols**: Implement SBE for Binance streams

## Project Structure

```
cplusplus-arbitrage/
├── CMakeLists.txt
├── README.md
├── configs/
│   └── bot.json
├── data/
├── include/
│   ├── common/
│   │   └── types.hpp
│   ├── config/
│   │   └── config.hpp
│   ├── execution/
│   │   ├── execution_engine.hpp
│   │   └── order.hpp
│   ├── market_data/
│   │   ├── binance_client.hpp
│   │   ├── order_book.hpp
│   │   ├── polymarket_client.hpp
│   │   └── ws_client_base.hpp
│   ├── persistence/
│   │   └── trade_ledger.hpp
│   ├── position/
│   │   └── position_manager.hpp
│   ├── risk/
│   │   └── risk_manager.hpp
│   ├── strategy/
│   │   └── strategy_base.hpp
│   ├── ui/
│   │   └── terminal_ui.hpp
│   └── utils/
│       ├── crypto.hpp
│       ├── metrics.hpp
│       └── time_utils.hpp
├── logs/
├── scripts/
├── src/
│   ├── config/
│   ├── execution/
│   ├── market_data/
│   ├── persistence/
│   ├── position/
│   ├── risk/
│   ├── strategy/
│   ├── tools/
│   ├── ui/
│   ├── utils/
│   └── main.cpp
└── tests/
    ├── test_fee_calculation.cpp
    ├── test_main.cpp
    ├── test_order_book.cpp
    ├── test_risk_manager.cpp
    └── test_underpricing.cpp
```

## License

This software is provided for educational purposes. Use at your own risk.

## Disclaimer

This is not financial advice. The authors are not responsible for any losses incurred from using this software. Always start with paper trading and understand the risks involved before trading with real money.
