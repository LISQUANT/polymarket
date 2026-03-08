#pragma once

#include "types.hpp"

#include <chrono>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

class LatencyTracker {
public:
    static constexpr size_t MAX_SAMPLES = 16384;

    void record(double value_us) {
        samples_[count_ & (MAX_SAMPLES - 1)] = value_us;
        ++count_;
    }

    void reset() { count_ = 0; }

    struct Stats {
        double min = 0.0;
        double max = 0.0;
        double avg = 0.0;
        double p50 = 0.0;
        double p99 = 0.0;
        double p999 = 0.0;
        uint64_t count = 0;
    };

    Stats compute() const {
        Stats s{};
        const uint64_t n = std::min<uint64_t>(count_, MAX_SAMPLES);
        s.count = count_;
        if (n == 0) return s;

        std::vector<double> sorted(samples_, samples_ + n);
        std::sort(sorted.begin(), sorted.end());

        s.min = sorted.front();
        s.max = sorted.back();

        double sum = 0.0;
        for (double v : sorted) sum += v;
        s.avg = sum / n;
        s.p50 = sorted[static_cast<size_t>(n * 0.50)];
        s.p99 = sorted[static_cast<size_t>(n * 0.99)];
        s.p999 = sorted[std::min(static_cast<size_t>(n * 0.999), static_cast<size_t>(n - 1))];
        return s;
    }

private:
    double samples_[MAX_SAMPLES] = {};
    uint64_t count_ = 0;
};

class Logger {
public:
    explicit Logger(const Config& config);
    ~Logger();

    void consume_message(uint32_t bytes);
    void consume_book(float parse_us, float book_us, float arb_us, float e2e_us, bool arb_checked);
    void consume_price_change(float parse_us, float book_us, float arb_us, float e2e_us, bool arb_checked);
    void consume_bbo(float parse_us, float book_us, float arb_us, float e2e_us, bool arb_checked);
    void consume_trade();
    void consume_opportunity(const ArbOpportunity& opp);

    void maybe_print_summary();
    void print_final_summary();

private:
    void print_summary(int interval_seconds);
    void reset_interval();

    int summary_interval_seconds_ = 60;
    bool metrics_enabled_ = true;
    bool flush_each_write_ = false;
    std::ofstream csv_file_;

    std::chrono::steady_clock::time_point interval_start_;

    uint64_t msg_count_ = 0;
    uint64_t book_count_ = 0;
    uint64_t price_change_count_ = 0;
    uint64_t bbo_count_ = 0;
    uint64_t trade_count_ = 0;
    uint64_t arb_checks_ = 0;
    uint64_t total_opps_ = 0;
    uint64_t buy_both_ = 0;
    uint64_t sell_both_ = 0;

    uint64_t interval_msgs_ = 0;
    uint64_t interval_books_ = 0;
    uint64_t interval_price_changes_ = 0;
    uint64_t interval_bbo_ = 0;
    uint64_t interval_trades_ = 0;

    double cumulative_pnl_ = 0.0;
    double cumulative_vol_ = 0.0;

    LatencyTracker lat_parse_;
    LatencyTracker lat_book_;
    LatencyTracker lat_arb_;
    LatencyTracker lat_e2e_;
    LatencyTracker msg_sizes_;
};
