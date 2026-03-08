#pragma once

#include "types.hpp"
#include <string>
#include <fstream>
#include <mutex>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cmath>

// Fixed-size ring buffer for latency samples — no heap allocation after init
class LatencyTracker {
public:
    static constexpr size_t MAX_SAMPLES = 16384;  // power of 2 for fast modulo

    void record(double value_us) {
        samples_[count_ & (MAX_SAMPLES - 1)] = value_us;
        count_++;
    }

    void reset() { count_ = 0; }
    uint64_t count() const { return count_; }

    // Compute percentiles — sorts a copy, only call from summary (not hot path)
    struct Stats {
        double min, max, avg, p50, p99, p999;
        uint64_t count;
    };

    Stats compute() const {
        Stats s{};
        uint64_t n = std::min(count_, (uint64_t)MAX_SAMPLES);
        s.count = count_;  // total recorded, not just in buffer
        if (n == 0) return s;

        // Copy to temp buffer for sorting
        std::vector<double> sorted(samples_, samples_ + n);
        std::sort(sorted.begin(), sorted.end());

        s.min = sorted[0];
        s.max = sorted[n - 1];

        double sum = 0;
        for (size_t i = 0; i < n; i++) sum += sorted[i];
        s.avg = sum / n;

        s.p50  = sorted[(size_t)(n * 0.50)];
        s.p99  = sorted[(size_t)(n * 0.99)];
        s.p999 = sorted[std::min((size_t)(n * 0.999), n - 1)];

        return s;
    }

private:
    double   samples_[MAX_SAMPLES] = {};
    uint64_t count_ = 0;
};

class Logger {
public:
    explicit Logger(const std::string& csv_path);
    ~Logger();

    void log_opportunity(const ArbOpportunity& opp);

    // Record latencies per pipeline stage (in microseconds)
    void record_parse_latency(double us)    { lat_parse_.record(us); }
    void record_book_latency(double us)     { lat_book_.record(us); }
    void record_arb_latency(double us)      { lat_arb_.record(us); }
    void record_e2e_latency(double us)      { lat_e2e_.record(us); }
    void record_msg_size(double bytes)      { msg_sizes_.record(bytes); }

    // Track message counts
    void record_message() { msg_count_++; }
    void record_book_event() { book_count_++; }
    void record_trade_event() { trade_count_++; }

    uint64_t msg_count() const { return msg_count_; }
    uint64_t book_count() const { return book_count_; }
    uint64_t trade_count() const { return trade_count_; }

    // Print full summary with latency percentiles, then reset interval counters
    void print_summary(int interval_seconds, uint64_t arb_checks,
                       uint64_t opportunities, uint64_t buy_both, uint64_t sell_both,
                       double cumulative_pnl = 0.0, double cumulative_vol = 0.0);

    // Reset per-interval latency trackers (called after summary)
    void reset_interval() {
        lat_parse_.reset();
        lat_book_.reset();
        lat_arb_.reset();
        lat_e2e_.reset();
        msg_sizes_.reset();
        interval_msgs_ = msg_count_.load();
        interval_books_ = book_count_.load();
        interval_trades_ = trade_count_.load();
    }

    // Store snapshot for interval delta
    void mark_interval_start() {
        interval_msgs_ = msg_count_.load();
        interval_books_ = book_count_.load();
        interval_trades_ = trade_count_.load();
    }

    uint64_t interval_msgs_delta() const { return msg_count_ - interval_msgs_; }
    uint64_t interval_books_delta() const { return book_count_ - interval_books_; }
    uint64_t interval_trades_delta() const { return trade_count_ - interval_trades_; }

private:
    std::ofstream csv_file_;
    std::mutex    csv_mutex_;

    std::atomic<uint64_t> msg_count_{0};
    std::atomic<uint64_t> book_count_{0};
    std::atomic<uint64_t> trade_count_{0};

    uint64_t interval_msgs_ = 0;
    uint64_t interval_books_ = 0;
    uint64_t interval_trades_ = 0;

    LatencyTracker lat_parse_;   // t0 → t1: ws recv to parse done
    LatencyTracker lat_book_;    // t1 → t2: parse done to book updated
    LatencyTracker lat_arb_;     // t2 → t3: book updated to arb check done
    LatencyTracker lat_e2e_;     // t0 → t3: full pipeline
    LatencyTracker msg_sizes_;   // raw message byte sizes
};
