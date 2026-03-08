#pragma once

#include "types.hpp"

class ArbitrageDetector {
public:
    explicit ArbitrageDetector(const Config& config);

    // Check for arbitrage on a contract after any book update
    // Returns true if opportunity found
    bool check(const Contract& contract, NanoTime t2_book_updated);

    // Get the last detected opportunity (valid only if check() returned true)
    const ArbOpportunity& last_opportunity() const { return last_opp_; }

    // Stats
    uint64_t total_checks() const { return total_checks_; }
    uint64_t total_opportunities() const { return total_opps_; }
    uint64_t buy_both_count() const { return buy_both_; }
    uint64_t sell_both_count() const { return sell_both_; }
    double   cumulative_pnl() const { return cumulative_pnl_; }
    double   cumulative_volume() const { return cumulative_vol_; }

private:
    const Config& config_;
    ArbOpportunity last_opp_;
    uint64_t total_checks_ = 0;
    uint64_t total_opps_   = 0;
    uint64_t buy_both_     = 0;
    uint64_t sell_both_    = 0;
    double   cumulative_pnl_ = 0.0;
    double   cumulative_vol_ = 0.0;
};
