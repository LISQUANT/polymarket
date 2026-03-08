#include "arbitrage.hpp"
#include <cstdio>
#include <algorithm>

ArbitrageDetector::ArbitrageDetector(const Config& config) : config_(config) {}

bool ArbitrageDetector::check(const Contract& contract, NanoTime t2_book_updated) {
    total_checks_++;

    const auto& yes = contract.book_yes;
    const auto& no  = contract.book_no;

    if (yes.best_ask == 0 || no.best_ask == 0 ||
        yes.best_bid == 0 || no.best_bid == 0) {
        return false;
    }

    // Fee: taker_fee_bps is in basis points of $1 (100 bps = 1 cent per side)
    // For BUY_BOTH we pay fees on both legs, so total fee = 2 * fee per leg
    // Polymarket fee is on the purchase amount, roughly fee_bps/10000 * cost per side
    // Simplified: total fee cost in our price units (×1000) = 2 * taker_fee_bps / 10
    // e.g., 100 bps fee on each leg = 2 * 100/10 = 20 units (= $0.020)
    int fee_cost = 2 * config_.taker_fee_bps / 10;

    bool found = false;

    // BUY-BOTH: buy YES at ask + buy NO at ask < $1.00
    {
        uint16_t cost = yes.best_ask + no.best_ask;
        int raw_edge = (int)PRICE_ONE - (int)cost;
        int net_edge = raw_edge - fee_cost;  // edge after fees

        if (net_edge > config_.min_edge_threshold_bps) {
            Size size = std::min(yes.best_ask_size, no.best_ask_size);

            last_opp_ = {};
            last_opp_.t2_book_updated_ns = t2_book_updated;
            last_opp_.t3_arb_checked_ns = now_ns();
            last_opp_.contract_name = contract.asset_name;
            last_opp_.arb_type = "BUY_BOTH";
            last_opp_.ask_yes = yes.best_ask;
            last_opp_.ask_no = no.best_ask;
            last_opp_.bid_yes = yes.best_bid;
            last_opp_.bid_no = no.best_bid;
            last_opp_.cost_or_proceeds = cost;
            last_opp_.edge_bps = net_edge;
            last_opp_.size_usd = size;
            last_opp_.theoretical_pnl = (double)net_edge / 1000.0 * size;

            cumulative_pnl_ += last_opp_.theoretical_pnl;
            cumulative_vol_ += (double)size;

            printf("[ARB] BUY_BOTH  | cost=%u | raw_edge=%d fee=%d net_edge=%d (%.1f%%) | size=$%u | pnl=$%.2f | cum_pnl=$%.2f\n",
                   cost, raw_edge, fee_cost, net_edge, net_edge / 10.0, size,
                   last_opp_.theoretical_pnl, cumulative_pnl_);

            total_opps_++;
            buy_both_++;
            found = true;
        } else {
            printf("[ARB CHECK] BUY_BOTH  | cost=%u | raw_edge=%d net=%d | NO_ARB\n",
                   cost, raw_edge, net_edge);
        }
    }

    // SELL-BOTH: sell YES at bid + sell NO at bid > $1.00
    {
        uint16_t proceeds = yes.best_bid + no.best_bid;
        int raw_edge = (int)proceeds - (int)PRICE_ONE;
        int net_edge = raw_edge - fee_cost;

        if (net_edge > config_.min_edge_threshold_bps) {
            Size size = std::min(yes.best_bid_size, no.best_bid_size);

            last_opp_ = {};
            last_opp_.t2_book_updated_ns = t2_book_updated;
            last_opp_.t3_arb_checked_ns = now_ns();
            last_opp_.contract_name = contract.asset_name;
            last_opp_.arb_type = "SELL_BOTH";
            last_opp_.ask_yes = yes.best_ask;
            last_opp_.ask_no = no.best_ask;
            last_opp_.bid_yes = yes.best_bid;
            last_opp_.bid_no = no.best_bid;
            last_opp_.cost_or_proceeds = proceeds;
            last_opp_.edge_bps = net_edge;
            last_opp_.size_usd = size;
            last_opp_.theoretical_pnl = (double)net_edge / 1000.0 * size;

            cumulative_pnl_ += last_opp_.theoretical_pnl;
            cumulative_vol_ += (double)size;

            printf("[ARB] SELL_BOTH | proceeds=%u | raw_edge=%d fee=%d net_edge=%d (%.1f%%) | size=$%u | pnl=$%.2f | cum_pnl=$%.2f\n",
                   proceeds, raw_edge, fee_cost, net_edge, net_edge / 10.0, size,
                   last_opp_.theoretical_pnl, cumulative_pnl_);

            total_opps_++;
            sell_both_++;
            found = true;
        } else {
            printf("[ARB CHECK] SELL_BOTH | proceeds=%u | raw_edge=%d net=%d | NO_ARB\n",
                   proceeds, raw_edge, net_edge);
        }
    }

    return found;
}
