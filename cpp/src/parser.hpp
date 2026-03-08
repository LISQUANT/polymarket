#pragma once

#include "types.hpp"
#include <simdjson.h>
#include <string_view>
#include <functional>

Price parse_price(std::string_view s);
Size parse_size(std::string_view s);

enum class EventType {
    BOOK,
    LAST_TRADE_PRICE,
    PRICE_CHANGE,
    TICK_SIZE_CHANGE,
    UNKNOWN
};

struct ParsedBookEvent {
    std::string_view asset_id;
    std::string_view market;
    uint64_t         timestamp = 0;
    bool             valid = false;
};

struct ParsedTradeEvent {
    std::string_view asset_id;
    std::string_view price_str;
    std::string_view side;
    std::string_view size_str;
    uint64_t         timestamp = 0;
};

class MessageParser {
public:
    MessageParser();

    using BookCallback  = std::function<void(const ParsedBookEvent&, simdjson::ondemand::object&)>;
    using TradeCallback = std::function<void(const ParsedTradeEvent&)>;

    // Zero-copy parse: takes raw pointer + length from beast buffer
    // Single memcpy into pre-allocated padded buffer for simdjson
    void parse(const char* data, size_t len, NanoTime t0_recv,
               BookCallback on_book, TradeCallback on_trade);

private:
    simdjson::ondemand::parser parser_;
    // Pre-allocated padded buffer — sized once at startup, reused every message
    static constexpr size_t BUF_CAPACITY = 256 * 1024;  // 256KB max message
    alignas(64) char padded_buf_[BUF_CAPACITY + simdjson::SIMDJSON_PADDING];
};
