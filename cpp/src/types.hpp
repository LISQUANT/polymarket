#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>

// Price stored as uint16_t × 1000 (e.g., $0.48 = 480)
using Price = uint16_t;
// Size stored as uint32_t whole units (share counts truncated to whole numbers)
using Size = uint32_t;
// Nanosecond timestamp
using NanoTime = uint64_t;

static constexpr Price PRICE_ONE = 1000;  // $1.00
static constexpr size_t PRICE_LEVEL_COUNT = PRICE_ONE + 1;
static constexpr int MAX_LEVELS = 50;

enum class ArbKind : uint8_t {
    BUY_BOTH = 0,
    SELL_BOTH = 1,
    BUY_GROUP_YES = 2,
    SELL_GROUP_YES = 3,
    BUY_NO_SELL_OTHERS = 4,
    SELL_NO_BUY_OTHERS = 5,
    MAKER_BUY_BOTH = 6,
    MAKER_SELL_BOTH = 7,
    MAKER_BUY_GROUP_YES = 8,
    MAKER_SELL_GROUP_YES = 9,
    MAKER_BUY_NO_SELL_OTHERS = 10,
    MAKER_SELL_NO_BUY_OTHERS = 11,
};

inline std::string_view arb_kind_name(ArbKind kind) noexcept {
    switch (kind) {
        case ArbKind::BUY_BOTH: return "BUY_BOTH";
        case ArbKind::SELL_BOTH: return "SELL_BOTH";
        case ArbKind::BUY_GROUP_YES: return "BUY_GROUP_YES";
        case ArbKind::SELL_GROUP_YES: return "SELL_GROUP_YES";
        case ArbKind::BUY_NO_SELL_OTHERS: return "BUY_NO_SELL_OTHERS";
        case ArbKind::SELL_NO_BUY_OTHERS: return "SELL_NO_BUY_OTHERS";
        case ArbKind::MAKER_BUY_BOTH: return "MAKER_BUY_BOTH";
        case ArbKind::MAKER_SELL_BOTH: return "MAKER_SELL_BOTH";
        case ArbKind::MAKER_BUY_GROUP_YES: return "MAKER_BUY_GROUP_YES";
        case ArbKind::MAKER_SELL_GROUP_YES: return "MAKER_SELL_GROUP_YES";
        case ArbKind::MAKER_BUY_NO_SELL_OTHERS: return "MAKER_BUY_NO_SELL_OTHERS";
        case ArbKind::MAKER_SELL_NO_BUY_OTHERS: return "MAKER_SELL_NO_BUY_OTHERS";
    }
    return "UNKNOWN";
}

inline bool is_maker_arb_kind(ArbKind kind) noexcept {
    switch (kind) {
        case ArbKind::MAKER_BUY_BOTH:
        case ArbKind::MAKER_SELL_BOTH:
        case ArbKind::MAKER_BUY_GROUP_YES:
        case ArbKind::MAKER_SELL_GROUP_YES:
        case ArbKind::MAKER_BUY_NO_SELL_OTHERS:
        case ArbKind::MAKER_SELL_NO_BUY_OTHERS:
            return true;
        default:
            return false;
    }
}

struct PriceLevel {
    Price price = 0;
    Size  size  = 0;
};

struct alignas(64) Orderbook {
    PriceLevel bids[MAX_LEVELS];
    PriceLevel asks[MAX_LEVELS];
    Size       bid_size_by_price[PRICE_LEVEL_COUNT] = {};
    Size       ask_size_by_price[PRICE_LEVEL_COUNT] = {};
    uint32_t   bid_epoch_by_price[PRICE_LEVEL_COUNT] = {};
    uint32_t   ask_epoch_by_price[PRICE_LEVEL_COUNT] = {};
    uint32_t   bid_epoch = 1;
    uint32_t   ask_epoch = 1;
    uint8_t    bid_count     = 0;
    uint8_t    ask_count     = 0;
    Price      best_bid      = 0;
    Price      best_ask      = 0;
    Size       best_bid_size = 0;
    Size       best_ask_size = 0;
    bool       has_snapshot  = false;
    uint64_t   timestamp     = 0;     // exchange timestamp
    NanoTime   local_update_ns = 0;   // our clock when processed
    uint64_t   touched_frame_id = 0;
};

struct Contract {
    std::string condition_id;
    std::string asset_name;
    std::string token_id_yes;
    std::string token_id_no;
    int         taker_fee_bps_override = -1;
    bool        market_metadata_loaded = false;
    bool        fee_schedule_enabled = false;
    double      fee_rate = 0.0;
    int         fee_exponent = 0;
    int         base_fee_bps = -1;
    bool        neg_risk = false;
    std::string event_slug;
    std::string event_title;
    uint64_t    touched_frame_id = 0;
    uint64_t    total_updates = 0;
    uint64_t    yes_updates = 0;
    uint64_t    no_updates = 0;
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
    ArbKind      arb_kind = ArbKind::BUY_BOTH;
    Price       ask_yes    = 0;
    Price       ask_no     = 0;
    Price       bid_yes    = 0;
    Price       bid_no     = 0;
    uint16_t    cost_or_proceeds = 0;
    int16_t     edge_bps   = 0;
    int16_t     raw_edge_bps = 0;
    Size        size_shares = 0;
    uint8_t     leg_count  = 2;
    double      theoretical_pnl = 0.0;
    double      paper_trade_pnl = 0.0;
    bool        counts_toward_paper_trade = false;
};

struct EdgeTelemetrySample {
    NanoTime         sample_time_ns = 0;
    std::string_view label;
    ArbKind          arb_kind = ArbKind::BUY_BOTH;
    uint16_t         reference_value = 0;
    uint8_t          leg_count = 0;
    int16_t          raw_edge_bps = 0;
    int16_t          net_edge_bps = 0;
    bool             valid = false;
};

struct ReplaySnapshot {
    uint64_t         frame_id = 0;
    NanoTime         t0_ws_recv_ns = 0;
    NanoTime         t1_parse_start_ns = 0;
    NanoTime         t2_parse_done_ns = 0;
    NanoTime         t3_books_done_ns = 0;
    uint32_t         frame_bytes = 0;
    uint16_t         book_events = 0;
    uint16_t         price_change_events = 0;
    uint16_t         bbo_events = 0;
    uint16_t         trade_events = 0;
    std::string_view event_key;
    std::string_view event_label;
    std::string_view contract_name;
    uint8_t          leg_index = 0;
    uint8_t          leg_count = 0;
    bool             touched_in_frame = false;
    uint64_t         yes_exchange_ts = 0;
    uint64_t         no_exchange_ts = 0;
    Price            yes_bid = 0;
    Size             yes_bid_size = 0;
    Price            yes_ask = 0;
    Size             yes_ask_size = 0;
    Price            no_bid = 0;
    Size             no_bid_size = 0;
    Price            no_ask = 0;
    Size             no_ask_size = 0;
};

struct Config {
    std::string websocket_host = "ws-subscriptions-clob.polymarket.com";
    std::string websocket_path = "/ws/market";
    uint16_t    websocket_port = 443;
    int         min_edge_threshold_bps = 0;
    int         taker_fee_bps = 100;
    int         summary_interval_seconds = 60;
    std::string log_file = "logs/arb_log.csv";
    std::string near_miss_log_file = "logs/near_miss.csv";
    std::string replay_log_file = "logs/replay_state.csv";
    int         warmup_seconds = 0;
    int         ping_interval_seconds = 15;
    int         stale_feed_timeout_seconds = 30;
    int         reconnect_max_delay_seconds = 30;
    size_t      message_queue_capacity = 128;
    size_t      metrics_queue_capacity = 8192;
    size_t      opportunity_queue_capacity = 1024;
    size_t      replay_queue_capacity = 8192;
    size_t      active_market_report_limit = 10;
    bool        custom_feature_enabled = true;
    bool        initial_dump = true;
    bool        metrics_enabled = true;
    bool        edge_telemetry_enabled = true;
    bool        replay_logging_enabled = true;
    bool        hot_path_logging = false;
    bool        flush_csv_each_write = false;
    bool        fetch_market_metadata = true;
    bool        enable_group_arbitrage = true;
    bool        auto_detect_exhaustive_groups = true;
    bool        maker_arb_enabled = true;
    int         pin_thread_cpu = -1;
    int         receiver_cpu = 2;
    int         parser_cpu = 4;
    int         logger_cpu = 6;
    bool        lock_memory = false;
    size_t      prefault_stack_kb = 64;
    int         realtime_priority = 0;
    int         receiver_priority = 0;
    int         parser_priority = 0;
    int         logger_priority = 0;
    struct ConfiguredGroup {
        std::string key;
        std::string display_name;
        std::vector<std::string> condition_ids;
        bool exhaustive = true;
    };
    std::vector<Contract> contracts;
    std::vector<ConfiguredGroup> configured_groups;
};

struct ContractGroup {
    std::string key;
    std::string display_name;
    std::vector<Contract*> contracts;
    bool exhaustive = false;
    uint64_t touched_frame_id = 0;
};

inline NanoTime now_ns() {
    auto tp = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
}
