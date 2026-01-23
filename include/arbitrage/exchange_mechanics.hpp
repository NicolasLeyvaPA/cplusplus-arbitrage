#pragma once

#include <string>
#include <map>
#include <cmath>
#include <algorithm>

namespace arb {

// ============================================================================
// EXCHANGE MECHANICS
//
// Different exchanges use fundamentally different funding rate mechanics.
// This creates STRUCTURAL dispersion that cannot be arbitraged away.
//
// From research analysis:
// - Binance: Dynamic 1h intervals under stress (desynchronizes from 8h venues)
// - dYdX: 0% interest baseline (structurally lower rates than CEXs)
// - Deribit: Options-informed mark price (forward curve dynamics)
// - OKX: Impact Price methodology (order book depth weighted)
// - BitMEX: Aggressive ADL, socialized losses
//
// These differences are ENGINEERED, not accidental.
// ============================================================================

enum class FundingInterval {
    HOURLY,           // dYdX default
    EIGHT_HOUR,       // Binance/OKX/BitMEX standard
    DYNAMIC,          // Binance under stress (switches to 1h)
    CONTINUOUS        // Deribit perpetual
};

enum class PremiumIndexMethod {
    TWAP_IMPACT,      // Binance: TWAP of Impact Bid/Ask
    IMPACT_PRICE,     // OKX: Order book depth weighted
    TWAP_NOTIONAL,    // dYdX: TWAP of Impact Notional (500 USDC base)
    OPTIONS_INFORMED, // Deribit: Put-Call Parity + EMA
    DECAYING_BASIS    // BitMEX: Decaying Fair Basis
};

enum class LiquidationMechanism {
    UNIFIED_MARGIN,   // Binance: Cross-asset margin, ADL backstop
    ISOLATED_CROSS,   // OKX: Both modes, partial liquidation
    ONCHAIN_ENGINE,   // dYdX: On-chain risk engine
    ROBUST_MARK,      // Deribit: Anti-manipulation mark price
    AGGRESSIVE_ADL    // BitMEX: Socialized losses
};

struct ExchangeMechanics {
    std::string venue;

    // Funding mechanics
    FundingInterval funding_interval{FundingInterval::EIGHT_HOUR};
    int64_t funding_period_seconds{28800};  // 8 hours default
    double interest_rate_baseline{0.0003};  // 0.03% daily (0.01% per 8h interval)

    // Clamps and caps
    double funding_rate_clamp{0.0005};      // ±0.05% clamp
    double funding_rate_cap{0.0075};        // 0.75% cap (Binance: 0.75 * MMR)

    // Premium calculation
    PremiumIndexMethod premium_method{PremiumIndexMethod::TWAP_IMPACT};
    int64_t premium_sample_window_ms{60000}; // 1 minute TWAP window

    // Liquidation
    LiquidationMechanism liquidation_mechanism{LiquidationMechanism::UNIFIED_MARGIN};
    bool has_adl{true};
    double insurance_fund_coverage{0.0};    // Current insurance fund as % of OI

    // Fees
    double maker_fee_bps{2.0};
    double taker_fee_bps{5.0};

    // Risk parameters
    double max_leverage{100.0};
    double initial_margin_rate{0.01};       // 1% = 100x
    double maintenance_margin_rate{0.005};  // 0.5%

    // Exchange-specific quirks
    bool dynamic_funding_interval{false};   // Binance stress behavior
    bool zero_interest_baseline{false};     // dYdX v4 behavior
    bool options_informed_mark{false};      // Deribit behavior
};

// ============================================================================
// EXCHANGE PROFILES (from research)
// ============================================================================

inline ExchangeMechanics get_binance_mechanics() {
    ExchangeMechanics m;
    m.venue = "binance";
    m.funding_interval = FundingInterval::EIGHT_HOUR;
    m.funding_period_seconds = 28800;
    m.interest_rate_baseline = 0.0003;      // 0.03% daily
    m.funding_rate_clamp = 0.0005;          // ±0.05%
    m.funding_rate_cap = 0.0075;            // 0.75% (0.75 * MMR)
    m.premium_method = PremiumIndexMethod::TWAP_IMPACT;
    m.liquidation_mechanism = LiquidationMechanism::UNIFIED_MARGIN;
    m.has_adl = true;
    m.dynamic_funding_interval = true;      // Switches to 1h under stress!
    m.maker_fee_bps = 2.0;
    m.taker_fee_bps = 4.0;
    return m;
}

inline ExchangeMechanics get_okx_mechanics() {
    ExchangeMechanics m;
    m.venue = "okx";
    m.funding_interval = FundingInterval::EIGHT_HOUR;
    m.funding_period_seconds = 28800;
    m.interest_rate_baseline = 0.0003;      // 0.03% daily
    m.funding_rate_clamp = 0.0005;          // ±0.05%
    m.premium_method = PremiumIndexMethod::IMPACT_PRICE;
    m.liquidation_mechanism = LiquidationMechanism::ISOLATED_CROSS;
    m.has_adl = true;
    m.maker_fee_bps = 2.0;
    m.taker_fee_bps = 5.0;
    return m;
}

inline ExchangeMechanics get_dydx_mechanics() {
    ExchangeMechanics m;
    m.venue = "dydx";
    m.funding_interval = FundingInterval::HOURLY;  // Hourly!
    m.funding_period_seconds = 3600;
    m.interest_rate_baseline = 0.0;         // 0% interest (v4 default)
    m.funding_rate_clamp = 0.0;             // Cap based on margin
    m.premium_method = PremiumIndexMethod::TWAP_NOTIONAL;
    m.liquidation_mechanism = LiquidationMechanism::ONCHAIN_ENGINE;
    m.has_adl = false;                      // On-chain = different model
    m.zero_interest_baseline = true;        // Structurally lower rates
    m.maker_fee_bps = 2.0;
    m.taker_fee_bps = 5.0;
    return m;
}

inline ExchangeMechanics get_deribit_mechanics() {
    ExchangeMechanics m;
    m.venue = "deribit";
    m.funding_interval = FundingInterval::CONTINUOUS;
    m.funding_period_seconds = 0;           // Continuous
    m.interest_rate_baseline = 0.0;
    m.funding_rate_cap = 0.005;             // ±0.5%
    m.premium_method = PremiumIndexMethod::OPTIONS_INFORMED;
    m.liquidation_mechanism = LiquidationMechanism::ROBUST_MARK;
    m.has_adl = true;
    m.options_informed_mark = true;         // Forward curve dynamics
    m.maker_fee_bps = 0.0;                  // Maker rebate
    m.taker_fee_bps = 5.0;
    return m;
}

inline ExchangeMechanics get_bitmex_mechanics() {
    ExchangeMechanics m;
    m.venue = "bitmex";
    m.funding_interval = FundingInterval::EIGHT_HOUR;
    m.funding_period_seconds = 28800;
    m.premium_method = PremiumIndexMethod::DECAYING_BASIS;
    m.liquidation_mechanism = LiquidationMechanism::AGGRESSIVE_ADL;
    m.has_adl = true;                       // Aggressive socialized losses
    m.maker_fee_bps = -2.5;                 // Maker rebate
    m.taker_fee_bps = 7.5;
    return m;
}

inline ExchangeMechanics get_bybit_mechanics() {
    ExchangeMechanics m;
    m.venue = "bybit";
    m.funding_interval = FundingInterval::EIGHT_HOUR;
    m.funding_period_seconds = 28800;
    m.interest_rate_baseline = 0.0003;
    m.funding_rate_clamp = 0.0005;
    m.premium_method = PremiumIndexMethod::TWAP_IMPACT;
    m.liquidation_mechanism = LiquidationMechanism::UNIFIED_MARGIN;
    m.has_adl = true;
    m.maker_fee_bps = 1.0;
    m.taker_fee_bps = 6.0;
    return m;
}

inline std::map<std::string, ExchangeMechanics> get_all_exchange_mechanics() {
    return {
        {"binance", get_binance_mechanics()},
        {"okx", get_okx_mechanics()},
        {"dydx", get_dydx_mechanics()},
        {"deribit", get_deribit_mechanics()},
        {"bitmex", get_bitmex_mechanics()},
        {"bybit", get_bybit_mechanics()}
    };
}

// ============================================================================
// DISPERSION ANALYSIS
//
// Why do these differences matter?
// ============================================================================

struct DispersionDriver {
    std::string venue_a;
    std::string venue_b;
    std::string driver;
    double expected_spread_bps;
    std::string risk_note;
};

inline std::vector<DispersionDriver> analyze_dispersion_drivers() {
    std::vector<DispersionDriver> drivers;

    // Binance dynamic interval vs fixed 8h venues
    drivers.push_back({
        "binance", "okx",
        "Dynamic 1h interval during stress desynchronizes from 8h venues",
        20.0,  // 20 bps expected during stress
        "Binance can switch to 1h funding without warning"
    });

    // dYdX 0% interest vs CEX 0.03% baseline
    drivers.push_back({
        "dydx", "binance",
        "dYdX 0% interest baseline vs CEX 0.03% daily creates permanent carry bias",
        10.0,  // 10 bps structural difference
        "dYdX rates structurally drift lower than CEXs"
    });

    // Deribit options-informed vs spot-based mark
    drivers.push_back({
        "deribit", "binance",
        "Options-informed mark reflects forward curve, not just spot-perp basis",
        15.0,
        "Deribit behaves differently during spot-led vs derivatives-led moves"
    });

    // CEX vs DEX tiered structure
    drivers.push_back({
        "binance", "dydx",
        "CEX dominates price discovery, DEX follows with lag",
        25.0,
        "Persistent spread at CEX-DEX boundary during volatility"
    });

    return drivers;
}

// ============================================================================
// FUNDING RATE NORMALIZATION
//
// Convert rates to common 8h basis for comparison
// ============================================================================

inline double normalize_to_8h(double rate, const ExchangeMechanics& mechanics) {
    switch (mechanics.funding_interval) {
        case FundingInterval::HOURLY:
            return rate * 8.0;  // 8 hours worth of hourly funding
        case FundingInterval::CONTINUOUS:
            // Approximate: continuous is per-second, multiply by 8h seconds
            return rate * 28800.0;
        case FundingInterval::DYNAMIC:
        case FundingInterval::EIGHT_HOUR:
        default:
            return rate;  // Already 8h
    }
}

inline double annualize_funding(double rate_8h) {
    // 3 funding periods per day * 365 days
    return rate_8h * 3.0 * 365.0;
}

// ============================================================================
// REGIME DETECTION
//
// Market regimes determine tradability of spreads
// ============================================================================

enum class MarketRegime {
    LOW_VOL_NORMAL,       // Best risk-adjusted returns
    EUPHORIC_BULL,        // High spreads, challenging execution
    DELEVERAGING_CRASH,   // Extreme spreads, liquidity evaporates
    CONSOLIDATION         // Low spreads, minimal opportunity
};

struct RegimeCharacteristics {
    MarketRegime regime;
    double typical_spread_bps;
    double spread_persistence_hours;
    double tradability_score;  // 0-1, higher = easier to trade
    std::string primary_risk;
};

inline RegimeCharacteristics get_regime_characteristics(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::LOW_VOL_NORMAL:
            return {
                regime,
                20.0,   // Moderate spreads
                24.0,   // High persistence
                0.9,    // Highly tradable
                "Transaction costs may consume edge"
            };
        case MarketRegime::EUPHORIC_BULL:
            return {
                regime,
                50.0,   // High spreads (20-30% annualized)
                72.0,   // Very persistent
                0.5,    // Challenging
                "Long squeeze risk, capital erosion on shorts"
            };
        case MarketRegime::DELEVERAGING_CRASH:
            return {
                regime,
                100.0,  // Extreme spreads
                4.0,    // Low persistence (hours)
                0.1,    // Dangerous - "phantom" liquidity
                "90%+ liquidity loss, frozen UIs, ADL"
            };
        case MarketRegime::CONSOLIDATION:
            return {
                regime,
                5.0,    // Low spreads
                48.0,   // High persistence (nothing happening)
                0.3,    // Low opportunity
                "Spread may not cover fees"
            };
    }
    return {MarketRegime::LOW_VOL_NORMAL, 0, 0, 0, ""};
}

}  // namespace arb
