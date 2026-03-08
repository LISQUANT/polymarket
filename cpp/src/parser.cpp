#include "parser.hpp"
#include <cstring>
#include <cstdio>

Price parse_price(std::string_view s) {
    if (s.empty()) return 0;

    size_t i = 0;
    if (s[0] == '0') i = 1;
    if (i < s.size() && s[i] == '.') i++;

    uint16_t result = 0;
    int digits = 0;
    while (i < s.size() && digits < 3) {
        if (s[i] >= '0' && s[i] <= '9') {
            result = result * 10 + (s[i] - '0');
            digits++;
        }
        i++;
    }
    while (digits < 3) {
        result *= 10;
        digits++;
    }
    return result;
}

Size parse_size(std::string_view s) {
    Size result = 0;
    for (char c : s) {
        if (c == '.') break;
        if (c >= '0' && c <= '9') {
            result = result * 10 + (c - '0');
        }
    }
    return result;
}

MessageParser::MessageParser() {
    // Zero the padding region once
    std::memset(padded_buf_ + BUF_CAPACITY, 0, simdjson::SIMDJSON_PADDING);
}

void MessageParser::parse(const char* data, size_t len, NanoTime t0_recv,
                          BookCallback on_book, TradeCallback on_trade) {
    if (len == 0 || len > BUF_CAPACITY) return;

    // Single memcpy into pre-allocated padded buffer — the ONLY copy on the hot path
    std::memcpy(padded_buf_, data, len);
    // Zero the padding after the data (simdjson requirement)
    std::memset(padded_buf_ + len, 0, simdjson::SIMDJSON_PADDING);

    // Zero-copy parse via padded_string_view — no allocation
    simdjson::padded_string_view psv(padded_buf_, len, len + simdjson::SIMDJSON_PADDING);

    simdjson::ondemand::document doc;
    auto error = parser_.iterate(psv).get(doc);
    if (error) {
        fprintf(stderr, "[PARSE ERROR] %s\n", simdjson::error_message(error));
        return;
    }

    simdjson::ondemand::array events;
    error = doc.get_array().get(events);
    if (error) return;

    for (auto elem : events) {
        simdjson::ondemand::object obj;
        if (elem.get_object().get(obj)) continue;

        std::string_view event_type;
        if (obj["event_type"].get_string().get(event_type)) continue;

        if (event_type == "book") {
            ParsedBookEvent ev;
            obj["asset_id"].get_string().get(ev.asset_id);
            obj["market"].get_string().get(ev.market);

            std::string_view ts_str;
            if (!obj["timestamp"].get_string().get(ts_str)) {
                ev.timestamp = 0;
                for (char c : ts_str) {
                    if (c >= '0' && c <= '9')
                        ev.timestamp = ev.timestamp * 10 + (c - '0');
                }
            }
            ev.valid = true;
            on_book(ev, obj);

        } else if (event_type == "last_trade_price") {
            ParsedTradeEvent ev;
            obj["asset_id"].get_string().get(ev.asset_id);
            obj["price"].get_string().get(ev.price_str);
            obj["side"].get_string().get(ev.side);
            obj["size"].get_string().get(ev.size_str);

            std::string_view ts_str;
            if (!obj["timestamp"].get_string().get(ts_str)) {
                ev.timestamp = 0;
                for (char c : ts_str) {
                    if (c >= '0' && c <= '9')
                        ev.timestamp = ev.timestamp * 10 + (c - '0');
                }
            }
            on_trade(ev);
        }
    }
}
