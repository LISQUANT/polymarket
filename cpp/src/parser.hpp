#pragma once

#include "types.hpp"
#include <simdjson.h>
#include <string_view>
#include <cstring>
#include <cstdio>

inline Price parse_price(std::string_view s) noexcept {
    if (s.empty()) return 0;

    uint32_t whole = 0;
    uint32_t frac = 0;
    int frac_digits = 0;
    bool seen_dot = false;

    for (char c : s) {
        if (c == '.') {
            seen_dot = true;
            continue;
        }
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (digit > 9) continue;
        if (!seen_dot) {
            whole = whole * 10 + digit;
        } else if (frac_digits < 3) {
            frac = frac * 10 + digit;
            ++frac_digits;
        }
    }

    while (frac_digits < 3) {
        frac *= 10;
        ++frac_digits;
    }

    const uint32_t value = whole * 1000 + frac;
    return static_cast<Price>(value <= PRICE_ONE ? value : PRICE_ONE);
}

inline Size parse_size(std::string_view s) noexcept {
    Size value = 0;
    for (char c : s) {
        if (c == '.') break;
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (digit <= 9) {
            value = value * 10 + digit;
        }
    }
    return value;
}

inline uint64_t parse_u64(std::string_view s) noexcept {
    uint64_t value = 0;
    for (char c : s) {
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (digit <= 9) {
            value = value * 10 + digit;
        }
    }
    return value;
}

enum class EventType {
    BOOK,
    LAST_TRADE_PRICE,
    PRICE_CHANGE,
    BEST_BID_ASK,
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

struct ParsedPriceChangeEvent {
    std::string_view asset_id;
    Price            price = 0;
    Size             size = 0;
    Price            best_bid = 0;
    Price            best_ask = 0;
    bool             is_bid = false;
    uint64_t         timestamp = 0;
};

struct ParsedBestBidAskEvent {
    std::string_view asset_id;
    Price            best_bid = 0;
    Price            best_ask = 0;
    uint64_t         timestamp = 0;
};

class MessageParser {
public:
    static constexpr size_t kBufferCapacity = 256 * 1024;

    MessageParser();

    template <typename BookCallback, typename TradeCallback,
              typename PriceChangeCallback, typename BestBidAskCallback>
    void parse(const char* data, size_t len, NanoTime /* t0_recv */,
               BookCallback&& on_book, TradeCallback&& on_trade,
               PriceChangeCallback&& on_price_change,
               BestBidAskCallback&& on_best_bid_ask) {
        if (len == 0 || len > kBufferCapacity) return;

        std::memcpy(padded_buf_, data, len);
        std::memset(padded_buf_ + len, 0, simdjson::SIMDJSON_PADDING);

        parse_padded(padded_buf_, len, kBufferCapacity + simdjson::SIMDJSON_PADDING,
                     std::forward<BookCallback>(on_book),
                     std::forward<TradeCallback>(on_trade),
                     std::forward<PriceChangeCallback>(on_price_change),
                     std::forward<BestBidAskCallback>(on_best_bid_ask));
    }

    template <typename BookCallback, typename TradeCallback,
              typename PriceChangeCallback, typename BestBidAskCallback>
    void parse_padded(const char* data, size_t len, size_t allocated,
                      BookCallback&& on_book, TradeCallback&& on_trade,
                      PriceChangeCallback&& on_price_change,
                      BestBidAskCallback&& on_best_bid_ask) {
        simdjson::padded_string_view psv(data, len, allocated);
        simdjson::ondemand::document doc;
        auto error = parser_.iterate(psv).get(doc);
        if (error) {
            std::fprintf(stderr, "[PARSE ERROR] %s\n", simdjson::error_message(error));
            return;
        }

        simdjson::ondemand::json_type root_type;
        if (doc.type().get(root_type)) return;

        if (root_type == simdjson::ondemand::json_type::array) {
            simdjson::ondemand::array events;
            if (doc.get_array().get(events)) return;
            for (auto elem : events) {
                simdjson::ondemand::object obj;
                if (elem.get_object().get(obj)) continue;
                parse_event_object(obj, on_book, on_trade, on_price_change, on_best_bid_ask);
            }
            return;
        }

        if (root_type == simdjson::ondemand::json_type::object) {
            simdjson::ondemand::object obj;
            if (doc.get_object().get(obj)) return;
            parse_event_object(obj, on_book, on_trade, on_price_change, on_best_bid_ask);
        }
    }

private:
    template <typename BookCallback, typename TradeCallback,
              typename PriceChangeCallback, typename BestBidAskCallback>
    static void parse_event_object(simdjson::ondemand::object& obj,
                                   BookCallback&& on_book, TradeCallback&& on_trade,
                                   PriceChangeCallback&& on_price_change,
                                   BestBidAskCallback&& on_best_bid_ask) {
        std::string_view event_type;
        if (obj.find_field_unordered("event_type").get_string().get(event_type)) return;

        if (event_type == "book") {
            ParsedBookEvent ev;
            if (obj.find_field_unordered("asset_id").get_string().get(ev.asset_id)) return;
            if (obj.find_field_unordered("market").get_string().get(ev.market)) return;

            std::string_view ts_str;
            if (!obj.find_field_unordered("timestamp").get_string().get(ts_str)) {
                ev.timestamp = parse_u64(ts_str);
            }
            ev.valid = true;
            on_book(ev, obj);
            return;
        }

        if (event_type == "last_trade_price") {
            ParsedTradeEvent ev;
            if (obj.find_field_unordered("asset_id").get_string().get(ev.asset_id)) return;
            if (obj.find_field_unordered("price").get_string().get(ev.price_str)) return;
            if (obj.find_field_unordered("side").get_string().get(ev.side)) return;
            if (obj.find_field_unordered("size").get_string().get(ev.size_str)) return;

            std::string_view ts_str;
            if (!obj.find_field_unordered("timestamp").get_string().get(ts_str)) {
                ev.timestamp = parse_u64(ts_str);
            }
            on_trade(ev);
            return;
        }

        if (event_type == "price_change") {
            std::string_view ts_str;
            const uint64_t timestamp =
                !obj.find_field_unordered("timestamp").get_string().get(ts_str) ? parse_u64(ts_str) : 0;

            simdjson::ondemand::array changes;
            if (obj.find_field_unordered("price_changes").get_array().get(changes)) return;

            for (auto change_elem : changes) {
                simdjson::ondemand::object change;
                if (change_elem.get_object().get(change)) continue;

                ParsedPriceChangeEvent ev;
                std::string_view sv;
                if (change.find_field_unordered("asset_id").get_string().get(ev.asset_id)) continue;
                if (change.find_field_unordered("price").get_string().get(sv)) continue;
                ev.price = parse_price(sv);
                if (change.find_field_unordered("size").get_string().get(sv)) continue;
                ev.size = parse_size(sv);
                if (change.find_field_unordered("side").get_string().get(sv)) continue;
                ev.is_bid = (sv == "BUY");
                if (change.find_field_unordered("best_bid").get_string().get(sv)) continue;
                ev.best_bid = parse_price(sv);
                if (change.find_field_unordered("best_ask").get_string().get(sv)) continue;
                ev.best_ask = parse_price(sv);
                ev.timestamp = timestamp;
                on_price_change(ev);
            }
            return;
        }

        if (event_type == "best_bid_ask") {
            ParsedBestBidAskEvent ev;
            std::string_view sv;
            if (obj.find_field_unordered("asset_id").get_string().get(ev.asset_id)) return;
            if (obj.find_field_unordered("best_bid").get_string().get(sv)) return;
            ev.best_bid = parse_price(sv);
            if (obj.find_field_unordered("best_ask").get_string().get(sv)) return;
            ev.best_ask = parse_price(sv);
            if (!obj.find_field_unordered("timestamp").get_string().get(sv)) {
                ev.timestamp = parse_u64(sv);
            }
            on_best_bid_ask(ev);
        }
    }

    simdjson::ondemand::parser parser_;
    alignas(64) char padded_buf_[kBufferCapacity + simdjson::SIMDJSON_PADDING];
};
