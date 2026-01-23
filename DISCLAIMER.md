# DISCLAIMER

## THIS SYSTEM CANNOT TRADE REAL MONEY PROFITABLY

This repository contains a **research and educational demonstration** of arbitrage concepts for binary outcome markets. It is **NOT** a profitable trading system.

### Critical Facts

1. **The demo simulator (demo_simulator_FAKE_DO_NOT_TRUST.cpp) manufactures fake opportunities**
   - Opportunities are injected 25% of the time
   - Real markets show opportunities 1-2% of the time (if at all)
   - Demo profits are fictional

2. **Paper trading uses adversarial assumptions but still cannot predict reality**
   - 40% fill rate (real may be lower on mispriced orders)
   - 1-3% adverse slippage
   - 20-60% partial fills
   - Even these cannot model competition, latency, or market microstructure

3. **Live mode has a fatal architectural flaw**
   - Paired orders are sent sequentially, not atomically
   - One leg may fill while the other does not
   - This creates naked directional exposure
   - Expected value is NEGATIVE

4. **The math is correct but irrelevant**
   - The arbitrage formulas are mathematically sound
   - But you cannot capture the edge faster than competition
   - And execution costs exceed theoretical edge

### What This Repository IS

- A demonstration of arbitrage concepts
- An educational tool for understanding binary markets
- A starting point for research
- Real market data connections to Binance and Polymarket

### What This Repository IS NOT

- A money-making system
- A profitable trading bot
- Suitable for live trading with real money
- A guarantee of any returns

### Do Not

- Trade real money with this system
- Trust the demo simulator's results
- Assume paper trading predicts real performance
- Use live mode without understanding non-atomic execution risk

### The Fundamental Problem

Even if the code were perfect, the opportunity does not exist in a form you can profitably capture:

1. Other traders are faster
2. Edges are smaller than costs
3. Atomic execution is not available
4. Your capital ($50) cannot absorb gas fees
5. Competition arbitrages away opportunities instantly

---

**See TRUTH.md for the complete forensic analysis.**

---

*"A system that admits it cannot work is infinitely more valuable than one that pretends it can."*
