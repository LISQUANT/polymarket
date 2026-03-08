#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>

// Price stored as uint16_t × 1000 (e.g., $0.48 = 480)
using Price = uint16_t;
// Size stored as uint32_t (whole dollars)
using Size = uint32_t;
// Nanosecond timestamp
using NanoTime = uint64_t;

static constexpr Price PRICE_ONE = 1000;  // $1.00
static constexpr size_t PRICE_LEVEL_COUNT = PRICE_ONE + 1;
static constexpr int MAX_LEVELS = 50;

struct PriceLevel {
    Price price = 0;
    Size  size  = 0;
};

struct alignas(64) Orderbook {
    PriceLevel bids[MAX_LEVELS];
    PriceLevel asks[MAX_LEVELS];
    Size       bid_size_by_price[PRICE_LEVEL_COUNT] = {};
    Size       ask_size_by_price[PRICE_LEVEL_COUNT] = {};
    uint8_t    bid_count     = 0;
    uint8_t    ask_count     = 0;
    Price      best_bid      = 0;
    Price      best_ask      = 0;
    Size       best_bid_size = 0;
    Size       best_ask_size = 0;
    bool       has_snapshot  = false;
    uint64_t   timestamp     = 0;     // exchange timestamp
    NanoTime   local_update_ns = 0;   // our clock when processed
};

struct Contract {
    std::string condition_id;
    std::string asset_name;
    std::string token_id_yes;
    std::string token_id_no;
    Orderbook   book_yes;
    Orderbook   book_no;
};

struct ArbOpportunity {
    NanoTime    t0_ws_recv_ns      = 0;
    NanoTime    t1_parse_done_ns   = 0;
    NanoTime    t2_book_updated_ns = 0;
    NanoTime    t3_arb_checked_ns  = 0;
    NanoTime    t4_logged_ns       = 0;
    std::string_view contract_name;
    std::string_view arb_type;  // "BUY_BOTH" or "SELL_BOTH"
    Price       ask_yes    = 0;
    Price       ask_no     = 0;
    Price       bid_yes    = 0;
    Price       bid_no     = 0;
    uint16_t    cost_or_proceeds = 0;
    int16_t     edge_bps   = 0;
    Size        size_usd   = 0;
    double      theoretical_pnl = 0.0;
};

struct Config {
    std::string websocket_host = "ws-subscriptions-clob.polymarket.com";
    std::string websocket_path = "/ws/market";
    uint16_t    websocket_port = 443;
    int         min_edge_threshold_bps = 0;
    int         taker_fee_bps = 100;
    int         summary_interval_seconds = 60;
    std::string log_file = "logs/arb_log.csv";
    int         ping_interval_seconds = 15;
    int         reconnect_max_delay_seconds = 30;
    bool        custom_feature_enabled = true;
    bool        initial_dump = true;
    bool        metrics_enabled = true;
    bool        hot_path_logging = false;
    bool        flush_csv_each_write = false;
    int         pin_thread_cpu = -1;
    int         receiver_cpu = 2;
    int         parser_cpu = 3;
    int         logger_cpu = 5;
    bool        lock_memory = false;
    size_t      prefault_stack_kb = 64;
    int         realtime_priority = 0;
    int         receiver_priority = 0;
    int         parser_priority = 0;
    int         logger_priority = 0;
    std::vector<Contract> contracts;
};

inline NanoTime now_ns() {
    auto tp = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
}
