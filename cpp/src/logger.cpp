#include "logger.hpp"
#include <cstdio>
#include <filesystem>

Logger::Logger(const std::string& csv_path) {
    auto dir = std::filesystem::path(csv_path).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir);
    }

    csv_file_.open(csv_path, std::ios::app);
    if (csv_file_.is_open()) {
        csv_file_.seekp(0, std::ios::end);
        if (csv_file_.tellp() == 0) {
            csv_file_ << "timestamp_ns,contract,arb_type,ask_yes,ask_no,bid_yes,bid_no,"
                       << "cost_or_proceeds,edge_bps,size_usd,theoretical_pnl,"
                       << "t0_recv_ns,t1_parse_ns,t2_book_ns,t3_arb_ns,t4_log_ns,"
                       << "parse_us,book_us,arb_us,e2e_us\n";
            csv_file_.flush();
        }
    }
}

Logger::~Logger() {
    if (csv_file_.is_open()) csv_file_.close();
}

void Logger::log_opportunity(const ArbOpportunity& opp) {
    if (!csv_file_.is_open()) return;

    double parse_us = (double)(opp.t1_parse_done_ns - opp.t0_ws_recv_ns) / 1000.0;
    double book_us  = (double)(opp.t2_book_updated_ns - opp.t1_parse_done_ns) / 1000.0;
    double arb_us   = (double)(opp.t3_arb_checked_ns - opp.t2_book_updated_ns) / 1000.0;
    double e2e_us   = (double)(opp.t3_arb_checked_ns - opp.t0_ws_recv_ns) / 1000.0;

    std::lock_guard<std::mutex> lock(csv_mutex_);
    csv_file_ << opp.t0_ws_recv_ns << ","
              << opp.contract_name << ","
              << opp.arb_type << ","
              << opp.ask_yes << ","
              << opp.ask_no << ","
              << opp.bid_yes << ","
              << opp.bid_no << ","
              << opp.cost_or_proceeds << ","
              << opp.edge_bps << ","
              << opp.size_usd << ","
              << opp.theoretical_pnl << ","
              << opp.t0_ws_recv_ns << ","
              << opp.t1_parse_done_ns << ","
              << opp.t2_book_updated_ns << ","
              << opp.t3_arb_checked_ns << ","
              << opp.t4_logged_ns << ","
              << parse_us << ","
              << book_us << ","
              << arb_us << ","
              << e2e_us << "\n";
    csv_file_.flush();
}

void Logger::print_summary(int interval_seconds, uint64_t arb_checks,
                           uint64_t opportunities, uint64_t buy_both, uint64_t sell_both,
                           double cumulative_pnl, double cumulative_vol) {
    uint64_t imsg = interval_msgs_delta();
    uint64_t ibook = interval_books_delta();
    uint64_t itrade = interval_trades_delta();
    double rate = (interval_seconds > 0) ? (double)imsg / interval_seconds : 0;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  SUMMARY (%ds interval)                                        ║\n", interval_seconds);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  Messages: %-6lu (%.1f/s) | Books: %-6lu | Trades: %-6lu     ║\n",
           imsg, rate, ibook, itrade);
    printf("║  Total:    %-6lu msgs   | %-6lu books  | %-6lu trades       ║\n",
           msg_count_.load(), book_count_.load(), trade_count_.load());
    printf("║  Arb checks: %-6lu | Opportunities: %-4lu (B:%lu S:%lu)        ║\n",
           arb_checks, opportunities, buy_both, sell_both);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  LATENCY (μs)          min      avg      p50      p99    p99.9 ║\n");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");

    auto print_lat = [](const char* label, const LatencyTracker& tracker) {
        auto s = tracker.compute();
        if (s.count == 0) {
            printf("║  %-18s   (no samples)                                    ║\n", label);
            return;
        }
        printf("║  %-18s %7.1f  %7.1f  %7.1f  %7.1f  %7.1f   ║\n",
               label, s.min, s.avg, s.p50, s.p99, s.p999);
    };

    print_lat("Parse (t0→t1)", lat_parse_);
    print_lat("Book upd (t1→t2)", lat_book_);
    print_lat("Arb check (t2→t3)", lat_arb_);
    print_lat("End-to-end (t0→t3)", lat_e2e_);

    auto sz = msg_sizes_.compute();
    if (sz.count > 0) {
        printf("╠══════════════════════════════════════════════════════════════════╣\n");
        printf("║  Msg size (bytes)    %7.0f  %7.0f  %7.0f  %7.0f  %7.0f   ║\n",
               sz.min, sz.avg, sz.p50, sz.p99, sz.p999);
    }

    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  PAPER TRADING                                                  ║\n");
    printf("║  Cumulative P&L: $%-10.2f | Volume: $%-10.0f               ║\n",
           cumulative_pnl, cumulative_vol);
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");
}
