#include "logger.hpp"

#include <cstdio>
#include <filesystem>

Logger::Logger(const Config& config)
    : summary_interval_seconds_(config.summary_interval_seconds)
    , metrics_enabled_(config.metrics_enabled)
    , flush_each_write_(config.flush_csv_each_write)
    , interval_start_(std::chrono::steady_clock::now()) {
    auto dir = std::filesystem::path(config.log_file).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir);
    }

    csv_file_.open(config.log_file, std::ios::app);
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
    if (csv_file_.is_open()) {
        csv_file_.flush();
        csv_file_.close();
    }
}

void Logger::consume_message(uint32_t bytes) {
    ++msg_count_;
    if (metrics_enabled_) {
        msg_sizes_.record(bytes);
    }
}

void Logger::consume_book(float parse_us, float book_us, float arb_us, float e2e_us, bool arb_checked) {
    ++book_count_;
    if (!metrics_enabled_) return;
    lat_parse_.record(parse_us);
    lat_book_.record(book_us);
    if (arb_checked) {
        lat_arb_.record(arb_us);
        lat_e2e_.record(e2e_us);
        ++arb_checks_;
    }
}

void Logger::consume_price_change(float parse_us, float book_us, float arb_us, float e2e_us, bool arb_checked) {
    ++price_change_count_;
    if (!metrics_enabled_) return;
    lat_parse_.record(parse_us);
    lat_book_.record(book_us);
    if (arb_checked) {
        lat_arb_.record(arb_us);
        lat_e2e_.record(e2e_us);
        ++arb_checks_;
    }
}

void Logger::consume_bbo(float parse_us, float book_us, float arb_us, float e2e_us, bool arb_checked) {
    ++bbo_count_;
    if (!metrics_enabled_) return;
    lat_parse_.record(parse_us);
    lat_book_.record(book_us);
    if (arb_checked) {
        lat_arb_.record(arb_us);
        lat_e2e_.record(e2e_us);
        ++arb_checks_;
    }
}

void Logger::consume_trade() {
    ++trade_count_;
}

void Logger::consume_opportunity(const ArbOpportunity& opp) {
    ++total_opps_;
    if (opp.arb_type == "BUY_BOTH") {
        ++buy_both_;
    } else if (opp.arb_type == "SELL_BOTH") {
        ++sell_both_;
    }
    cumulative_pnl_ += opp.theoretical_pnl;
    cumulative_vol_ += static_cast<double>(opp.size_usd);

    if (!csv_file_.is_open()) return;

    const double parse_us = static_cast<double>(opp.t1_parse_done_ns - opp.t0_ws_recv_ns) / 1000.0;
    const double book_us = static_cast<double>(opp.t2_book_updated_ns - opp.t1_parse_done_ns) / 1000.0;
    const double arb_us = static_cast<double>(opp.t3_arb_checked_ns - opp.t2_book_updated_ns) / 1000.0;
    const double e2e_us = static_cast<double>(opp.t3_arb_checked_ns - opp.t0_ws_recv_ns) / 1000.0;

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

    if (flush_each_write_) {
        csv_file_.flush();
    }
}

void Logger::maybe_print_summary() {
    if (!metrics_enabled_) return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - interval_start_).count();
    if (elapsed >= summary_interval_seconds_) {
        print_summary(static_cast<int>(elapsed));
        reset_interval();
        interval_start_ = now;
    }
}

void Logger::print_final_summary() {
    if (!metrics_enabled_) return;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - interval_start_).count();
    print_summary(static_cast<int>(elapsed));
    if (csv_file_.is_open()) {
        csv_file_.flush();
    }
}

void Logger::reset_interval() {
    lat_parse_.reset();
    lat_book_.reset();
    lat_arb_.reset();
    lat_e2e_.reset();
    msg_sizes_.reset();

    interval_msgs_ = msg_count_;
    interval_books_ = book_count_;
    interval_price_changes_ = price_change_count_;
    interval_bbo_ = bbo_count_;
    interval_trades_ = trade_count_;
}

void Logger::print_summary(int interval_seconds) {
    const uint64_t imsg = msg_count_ - interval_msgs_;
    const uint64_t ibook = book_count_ - interval_books_;
    const uint64_t ipx = price_change_count_ - interval_price_changes_;
    const uint64_t ibbo = bbo_count_ - interval_bbo_;
    const uint64_t itrade = trade_count_ - interval_trades_;
    const double rate = interval_seconds > 0 ? static_cast<double>(imsg) / interval_seconds : 0.0;

    std::printf("\n");
    std::printf("╔══════════════════════════════════════════════════════════════════╗\n");
    std::printf("║  SUMMARY (%ds interval)                                        ║\n", interval_seconds);
    std::printf("╠══════════════════════════════════════════════════════════════════╣\n");
    std::printf("║  Messages: %-6lu (%.1f/s)                                      ║\n", imsg, rate);
    std::printf("║  Books: %-6lu | PxChg: %-6lu | BBO: %-6lu | Trades: %-6lu ║\n",
                ibook, ipx, ibbo, itrade);
    std::printf("║  Total: %-6lu msgs | %-6lu books | %-6lu pxchg | %-6lu bbo ║\n",
                msg_count_, book_count_, price_change_count_, bbo_count_);
    std::printf("║         %-6lu trades                                          ║\n", trade_count_);
    std::printf("║  Arb checks: %-6lu | Opportunities: %-4lu (B:%lu S:%lu)        ║\n",
                arb_checks_, total_opps_, buy_both_, sell_both_);
    std::printf("╠══════════════════════════════════════════════════════════════════╣\n");
    std::printf("║  LATENCY (μs)          min      avg      p50      p99    p99.9 ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════╣\n");

    auto print_lat = [](const char* label, const LatencyTracker& tracker) {
        const auto s = tracker.compute();
        if (s.count == 0) {
            std::printf("║  %-18s   (no samples)                                    ║\n", label);
            return;
        }
        std::printf("║  %-18s %7.1f  %7.1f  %7.1f  %7.1f  %7.1f   ║\n",
                    label, s.min, s.avg, s.p50, s.p99, s.p999);
    };

    print_lat("Parse (t0→t1)", lat_parse_);
    print_lat("Book upd (t1→t2)", lat_book_);
    print_lat("Arb check (t2→t3)", lat_arb_);
    print_lat("End-to-end (t0→t3)", lat_e2e_);

    const auto sz = msg_sizes_.compute();
    if (sz.count > 0) {
        std::printf("╠══════════════════════════════════════════════════════════════════╣\n");
        std::printf("║  Msg size (bytes)    %7.0f  %7.0f  %7.0f  %7.0f  %7.0f   ║\n",
                    sz.min, sz.avg, sz.p50, sz.p99, sz.p999);
    }

    std::printf("╠══════════════════════════════════════════════════════════════════╣\n");
    std::printf("║  PAPER TRADING                                                  ║\n");
    std::printf("║  Cumulative P&L: $%-10.2f | Volume: $%-10.0f               ║\n",
                cumulative_pnl_, cumulative_vol_);
    std::printf("╚══════════════════════════════════════════════════════════════════╝\n\n");
}
