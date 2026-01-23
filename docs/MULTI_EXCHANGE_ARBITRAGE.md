# Multi-Exchange Arbitrage Architecture

## Overview

This document outlines the mathematical framework and architecture for cross-exchange
arbitrage. **This is a research document** - practical implementation faces significant
challenges detailed at the end.

## Strategy Categories

### S1: Cross-Exchange Spot Arbitrage

**Math:**
```
profit = (price_B - price_A) - fee_A - fee_B - transfer_cost
```

**Requirements:**
- Pre-funded accounts on both exchanges
- Real-time price feeds from both
- Fast execution API access

**Example:**
```
Binance BTC/USDT ask: $89,000
Kraken BTC/USD bid:   $89,180

Gross spread: $180 (0.20%)
Binance taker fee: 0.10% = $89
Kraken taker fee:  0.16% = $143
Net: $180 - $89 - $143 = -$52 (LOSS)

Need spread > 0.26% to profit on this pair
```

### S2: Triangular Arbitrage (Single Exchange)

**Math:**
```
Given three pairs: A/B, B/C, A/C
Rate_AB * Rate_BC * Rate_CA > 1.0 means profit

Example with BTC, ETH, USDT:
- BTC/USDT = 89000 (sell BTC, get USDT)
- ETH/USDT = 3100 (buy ETH with USDT)
- BTC/ETH = 28.7 (buy BTC with ETH)

Start: 1 BTC
1 BTC → 89000 USDT → 28.71 ETH → 1.0003 BTC
Profit: 0.03% per cycle (before fees)

With 0.1% fee per trade (3 trades):
Net: 0.03% - 0.30% = -0.27% (LOSS)

Need > 0.3% triangular discrepancy to profit
```

### S3: Futures-Spot Basis Arbitrage

**Math:**
```
basis = (futures_price - spot_price) / spot_price
annualized_yield = basis * (365 / days_to_expiry)

If annualized_yield > funding_cost, profitable
```

**Example:**
```
BTC Spot: $89,000
BTC 30-day Future: $89,800
Basis: 0.90%
Annualized: 0.90% * 12 = 10.8%

Strategy:
- Buy 1 BTC spot ($89,000)
- Sell 1 BTC future ($89,800)
- At expiry, deliver spot to close future
- Profit: $800 (0.9%) over 30 days

Risk: Need margin for futures, basis can widen before converging
```

### S4: Statistical Arbitrage (Mean Reversion)

**Math:**
```
spread = log(price_A) - beta * log(price_B)
z_score = (spread - mean_spread) / std_spread

If |z_score| > threshold:
  z > 2: Short A, Long B (spread will contract)
  z < -2: Long A, Short B (spread will expand)
```

**Cointegration test required** - assets must have long-term equilibrium relationship.

### S5: Latency Arbitrage

**Math:**
```
If exchange_A updates price at T
And exchange_B updates price at T + delta_t
During delta_t, stale price on B can be arbitraged
```

**Requirements:**
- Co-located servers at both exchanges
- Sub-millisecond execution
- Direct market data feeds (not REST API)

## Multi-Exchange Data Normalization

```cpp
// Unified order book structure
struct NormalizedQuote {
    std::string exchange;       // "binance", "kraken", "okx"
    std::string base_asset;     // "BTC"
    std::string quote_asset;    // "USDT"
    double bid_price;
    double bid_size;
    double ask_price;
    double ask_size;
    uint64_t timestamp_us;      // Microsecond precision
    double taker_fee_bps;       // Fee in basis points
};

// Cross-exchange opportunity
struct ArbitrageOpportunity {
    std::string buy_exchange;
    std::string sell_exchange;
    std::string asset;
    double buy_price;           // Best ask on buy exchange
    double sell_price;          // Best bid on sell exchange
    double gross_spread_bps;    // (sell - buy) / buy * 10000
    double net_spread_bps;      // After fees
    double available_size;      // Min of both sides
    double expected_profit;     // net_spread * size
    uint64_t detected_at_us;
    uint64_t staleness_us;      // Max quote age
};
```

## Exchange Fee Comparison

| Exchange | Maker Fee | Taker Fee | Withdrawal (BTC) | Notes |
|----------|-----------|-----------|------------------|-------|
| Binance | 0.10% | 0.10% | 0.0002 | VIP tiers lower |
| Kraken | 0.16% | 0.26% | 0.00015 | |
| OKX | 0.08% | 0.10% | 0.0002 | |
| Bybit | 0.10% | 0.10% | 0.0002 | |
| Coinbase | 0.40% | 0.60% | Network fee | High fees |
| KuCoin | 0.10% | 0.10% | 0.0004 | |
| Gate.io | 0.20% | 0.20% | 0.001 | |
| Huobi | 0.20% | 0.20% | 0.0001 | Now HTX |
| MEXC | 0.00% | 0.05% | 0.0003 | Zero maker |

**Minimum spread needed to profit (taker both sides):**
- Binance ↔ OKX: 0.20% (0.10% + 0.10%)
- Binance ↔ Kraken: 0.36% (0.10% + 0.26%)
- Any pair with Coinbase: >0.70%

## Capital Requirements

For cross-exchange arbitrage without transfers:

```
Required capital = 2 * max_position_size * max_price

Example for $10,000 max position in BTC at $89,000:
- Need ~0.11 BTC on Exchange A
- Need ~0.11 BTC worth of USDT on Exchange B
- Total capital: ~$20,000 split across exchanges

Problem: Capital is fragmented and earns nothing while waiting
```

## Latency Considerations

| Data Source | Typical Latency | Notes |
|-------------|-----------------|-------|
| REST API | 50-200ms | Too slow for arb |
| WebSocket | 5-50ms | Minimum viable |
| FIX Protocol | 1-5ms | Requires special access |
| Co-location | <1ms | $10k+/month |

**Reality:** With 50ms WebSocket latency on each exchange, your round-trip
detection-to-execution is ~200ms. Professional HFT firms operate at <1ms.

## Practical Challenges

### 1. Capital Efficiency
- Money sits idle on exchanges waiting for opportunities
- Rebalancing requires withdrawals (hours to days)
- Opportunity cost of locked capital

### 2. Counterparty Risk
- Exchanges can freeze withdrawals (FTX, others)
- Regulatory seizure risk
- Hack risk

### 3. Execution Risk
- Price moves during execution window
- Partial fills leave you exposed
- API rate limits during high volatility

### 4. Regulatory Issues
- Many exchanges don't serve all jurisdictions
- Tax implications of multi-exchange trading
- KYC requirements vary

### 5. Competition
- Professional firms have:
  - Co-located servers
  - Direct market access
  - $100M+ capital
  - PhD quant teams
- Retail cannot compete on speed

## Realistic Opportunities

### What MIGHT work for retail:

1. **Long-duration basis trades** (days/weeks, not seconds)
   - Futures basis when it's abnormally high
   - Requires margin and patience

2. **Funding rate arbitrage**
   - Perpetual futures have periodic funding
   - When funding is very negative, long perp + short spot
   - 8-hour cycles, not milliseconds

3. **Cross-chain DEX arbitrage**
   - Uniswap/Sushiswap price differences
   - MEV bots dominate, but occasional opportunities
   - Gas costs often exceed profit

4. **Illiquid market arbitrage**
   - Small-cap tokens with thin books
   - Higher spreads, lower competition
   - Higher risk (rug pulls, delistings)

### What does NOT work for retail:

1. ❌ Cross-exchange BTC/ETH spot arbitrage (too competitive)
2. ❌ Triangular arbitrage on major exchanges (too fast)
3. ❌ Latency arbitrage (need co-location)
4. ❌ Market making (need inventory management expertise)

## Conclusion

The math is straightforward. The execution is nearly impossible for retail traders
competing against professional firms with:
- 1000x faster infrastructure
- 1000x more capital
- Teams of PhDs optimizing every microsecond

**Realistic edge for retail:** Focus on longer time horizons (hours/days),
less liquid markets, and strategies that don't require speed.
