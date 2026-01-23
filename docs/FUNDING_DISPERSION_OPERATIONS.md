# Funding Rate Dispersion: Operational Manual

## The Only Document You Need To Execute This Strategy

---

## Part 1: Exact Entry Math

### Step 1: Compute the Spread

```
spread(t) = max_i(funding_rate_i(t)) - min_j(funding_rate_j(t))
```

Example at time T:
| Exchange | Funding Rate |
|----------|-------------|
| Binance  | +0.0300%    |
| Bybit    | +0.0250%    |
| OKX      | +0.0100%    |
| dYdX     | -0.0050%    |

```
spread = 0.0300% - (-0.0050%) = 0.0350% per 8h period
```

### Step 2: Compute Expected Annual Return

```
annual_return_gross = spread × periods_per_day × days_per_year × leverage
                    = 0.0350% × 3 × 365 × 2
                    = 76.6% annual
```

### Step 3: Compute Expected Losses

From failure mode analysis:

| Failure Mode | P(occur) | E[loss|occur] | E[loss] |
|--------------|----------|---------------|---------|
| Basis divergence | 30% | 2% | 0.6% |
| Funding flip | 50% | 0.5% | 0.25% |
| Exchange halt | 5% | 25% | 1.25% |
| Liquidation cascade | 10% | 5% | 0.5% |

**Total expected loss: 2.6% annually**

### Step 4: Compute Net Expected Return

```
annual_return_net = annual_return_gross - expected_loss - fees
                  = 76.6% - 2.6% - 1.0%  (assuming 1% annual fees)
                  = 73.0%
```

### Step 5: Entry Decision

**ENTER IF AND ONLY IF:**
```
annual_return_net > threshold  (default: 15%)
AND sharpe_estimate > 2.0
AND persistence_probability > 60%
```

---

## Part 2: Exact Position Construction

### The Graph Allocation Problem

Given N exchanges, find positions p_1, ..., p_N that maximize:

```
Objective: Σ p_i × funding_i  (total funding received)

Subject to:
  Σ p_i = 0                    (delta neutral)
  |p_i| ≤ max_per_exchange     (concentration limit)
  Σ |p_i| ≤ max_leverage × capital  (total leverage limit)
```

### Greedy Algorithm (Sufficient for N < 10)

```python
def allocate(exchanges, capital, max_per_ex=0.25, max_leverage=2.0):
    # Sort by funding rate
    sorted_ex = sorted(exchanges, key=lambda x: x.funding, reverse=True)

    positions = {}
    max_pos = capital * max_per_ex

    # Short top half (receive high funding)
    for ex in sorted_ex[:len(sorted_ex)//2]:
        positions[ex.name] = -min(max_pos, ex.max_position)

    # Long bottom half (pay low/negative funding)
    for ex in sorted_ex[len(sorted_ex)//2:]:
        positions[ex.name] = +min(max_pos, ex.max_position)

    # Scale to ensure delta neutral
    total_short = sum(abs(p) for p in positions.values() if p < 0)
    total_long = sum(p for p in positions.values() if p > 0)

    if total_long != total_short:
        scale = total_short / total_long
        for ex, pos in positions.items():
            if pos > 0:
                positions[ex] = pos * scale

    return positions
```

### Example

Capital: $100,000
Max per exchange: 25% = $25,000
Max leverage: 2x = $200,000 total position

| Exchange | Funding | Position | Funding Flow |
|----------|---------|----------|--------------|
| Binance  | +0.030% | -$25,000 (SHORT) | +$7.50/period |
| Bybit    | +0.025% | -$25,000 (SHORT) | +$6.25/period |
| OKX      | +0.010% | +$25,000 (LONG)  | -$2.50/period |
| dYdX     | -0.005% | +$25,000 (LONG)  | +$1.25/period |

**Net position: $0 (delta neutral)**
**Net funding per period: +$12.50**
**Annual at this rate: $12.50 × 3 × 365 = $13,687.50 = 13.7% on $100k**

---

## Part 3: Exact Exit Math

### Kill Condition 1: Spread Collapse

**EXIT IMMEDIATELY IF:**
```
spread(t) < 0.005%  (annualized ~5.5%)
```

At this level, fees eat the edge.

### Kill Condition 2: Funding Flip

**EXIT WITHIN 1 PERIOD IF:**
```
We are SHORT on exchange E AND funding_E < -0.01%
OR
We are LONG on exchange E AND funding_E > 0.05%
```

This means we're now paying on the wrong side.

### Kill Condition 3: Basis Divergence

**EXIT IMMEDIATELY IF:**
```
|basis_A - basis_B| > 0.5%
```

Where basis = perp_price - spot_price.

This indicates exchange-specific stress.

### Kill Condition 4: Drawdown Limit

**EXIT IMMEDIATELY IF:**
```
position_pnl < -3% of position value
```

Cut losses before they compound.

### Kill Condition 5: Exchange Distress

**EXIT IMMEDIATELY IF:**
- Withdrawal delays > 24h on any exchange with position
- Trading halted
- Unusual spread between exchange's perp and others' perps

---

## Part 4: How Trades Actually Fail

### Failure Mode 1: Basis Divergence

**What happens:**
Exchange A's perp trades at $89,000
Exchange B's perp trades at $89,450
You're short A, long B
Your MTM shows -$450 loss per BTC

**Why it happens:**
- Liquidity crunch on one exchange
- Regional demand differences
- Liquidation cascade on one venue

**How bad:**
- Usually <1% and temporary
- Can reach 2-3% in crisis
- Rare to exceed 5%

**Mitigation:**
- Exit if divergence > 0.5%
- Size smaller on less liquid exchanges

### Failure Mode 2: Funding Flip

**What happens:**
You're short Binance (receiving +0.03% funding)
Market sentiment shifts
Binance funding goes to -0.02%
Now you're PAYING 0.02% instead of receiving

**Why it happens:**
- Sentiment shift (BTC dumps, shorts dominate)
- Deleveraging event
- Large OI unwind

**How bad:**
- If you exit promptly: <0.5% loss
- If you hold hoping for reversion: can compound

**Mitigation:**
- Monitor every funding period
- Exit when your "receive" position flips to "pay"
- Accept the small loss, don't hope

### Failure Mode 3: Exchange Failure

**What happens:**
You have $25,000 position on Exchange X
Exchange X halts withdrawals
Your funds are frozen

**Why it happens:**
- Hack
- Regulatory action
- Insolvency (FTX-style)

**How bad:**
- 50-100% loss on that exchange
- If 25% of capital there: 12.5-25% total loss

**Mitigation:**
- Never >25% on any exchange
- Prefer established exchanges (Binance, OKX, Bybit)
- Monitor exchange news actively
- Consider exchange risk as direct cost

### Failure Mode 4: Liquidation Cascade

**What happens:**
BTC drops 15% in an hour
Your long perp positions get margin called
You're liquidated at bad prices
Your short positions profit, but less than long losses

**Why it happens:**
- Black swan price move
- Flash crash
- Coordinated manipulation

**How bad:**
- Depends on leverage
- At 2x: can survive 20%+ moves
- At 5x: 10% move can liquidate

**Mitigation:**
- Keep leverage ≤ 2x per side
- Maintain 50% margin buffer
- Set stop losses independent of liquidation

### Failure Mode 5: Spread Collapse (Crowding)

**What happens:**
Strategy becomes popular
Everyone piles into same trade
Funding dispersion shrinks
Expected returns drop to 5%

**Why it happens:**
- Alpha decay is real
- Capital flows to returns
- Strategy capacity is finite

**How bad:**
- No loss, just reduced returns
- May not be worth the operational overhead

**Mitigation:**
- Accept that edge decays
- Don't oversize based on historical returns
- Have a minimum return threshold to continue

---

## Part 5: Operational Checklist

### Daily Operations

```
□ Check funding rates on all exchanges (1h before funding)
□ Verify positions match target allocation
□ Check basis divergence across exchanges
□ Review any exchange news/announcements
□ Confirm margin levels >50% on all exchanges
```

### At Each Funding Period (Every 8h)

```
□ Record funding payment received/paid
□ Recalculate spread
□ Check kill conditions
□ If spread < threshold: prepare exit
□ If funding flipped: exit that leg
```

### Weekly Operations

```
□ Rebalance capital across exchanges if needed
□ Review cumulative P&L vs expected
□ Check for new exchanges to add to graph
□ Review failure mode probabilities based on recent events
□ Assess crowding (is spread compressing over time?)
```

### Monthly Operations

```
□ Full reconciliation of all positions
□ Review performance vs benchmark (risk-free rate + spread history)
□ Adjust position sizes based on updated risk estimates
□ Tax lot tracking for realized gains
```

---

## Part 6: Position Sizing

### Kelly Criterion (Modified)

Classic Kelly:
```
f* = (μ - r) / σ²
```

Where:
- μ = expected annual return
- r = risk-free rate
- σ = annual volatility

For funding dispersion with:
- μ = 30% expected
- r = 5% risk-free
- σ = 5% annual volatility (basis + funding vol)

```
f* = (0.30 - 0.05) / 0.05² = 100
```

This says "lever 100x" which is insane.

### Why Kelly Overcalls

Kelly assumes:
- You can always rebalance
- No transaction costs
- Continuous trading
- Known distribution

None of these hold. Use **HALF-KELLY at most**.

### Practical Sizing

```
position_size = min(
    capital × half_kelly_fraction,
    capital × max_leverage,
    exchange_position_limit
)
```

For $100k capital:
```
half_kelly = 50 (still too high)
max_leverage = 2.0
actual_fraction = min(50, 2.0) = 2.0

Total position = $100k × 2.0 = $200k across all exchanges
Per exchange = $200k / 4 = $50k (or 25% of capital)
```

---

## Part 7: Expected Performance Summary

### Base Case (Conservative)

- Spread: 0.015% per period
- Leverage: 2x
- Exchanges: 4
- Annual gross: 32.9%
- Expected losses: 2.6%
- Fees: 1%
- **Net annual: ~29%**
- **Sharpe: ~8**

### Bull Case (High Spread Environment)

- Spread: 0.035% per period
- Leverage: 2x
- Exchanges: 4
- Annual gross: 76.6%
- Expected losses: 3%
- Fees: 1%
- **Net annual: ~72%**
- **Sharpe: ~12**

### Bear Case (Spread Compression)

- Spread: 0.008% per period
- Leverage: 2x
- Exchanges: 4
- Annual gross: 17.5%
- Expected losses: 2%
- Fees: 1%
- **Net annual: ~14%**
- **Sharpe: ~4**

### Worst Case (Everything Goes Wrong)

- Exchange failure (5% prob): -25% one exchange = -6.25% total
- Basis divergence (realized): -2%
- Funding flip (multiple periods before exit): -1%
- **Total worst case: ~-9%**

---

## Part 8: Implementation Requirements

### Minimum Infrastructure

1. **Exchange Accounts:** Binance, Bybit, OKX (minimum 3)
2. **API Access:** REST for positions, WebSocket for funding rates
3. **Capital:** $10,000+ (for meaningful absolute returns)
4. **Monitoring:** Ability to check positions every 8 hours
5. **Alerts:** Automated alerts for kill conditions

### Optional Enhancements

1. **Automated execution:** Scripts to enter/exit positions
2. **Real-time monitoring:** Dashboard showing all positions and P&L
3. **Backtesting:** Historical analysis of funding spreads
4. **Risk management:** Automated position cuts on kill conditions

### NOT Required

1. Low-latency infrastructure
2. Co-location
3. Market data feeds faster than 1 second
4. Complex execution algorithms

---

## Final Word

This strategy works because:

1. **Funding dispersion is structural, not temporary**
2. **It operates on timescales where speed doesn't matter**
3. **The edge is persistent because constraints persist**
4. **Risk is quantifiable and manageable**

It doesn't work if:

1. **You overleverage** (liquidation cascade will kill you)
2. **You concentrate on one exchange** (exchange failure will kill you)
3. **You ignore kill conditions** (losses will compound)
4. **You expect 100% win rate** (there will be losing periods)

Execute with discipline. Manage risk. Harvest the structural rent.
