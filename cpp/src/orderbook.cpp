#include "orderbook.hpp"
#include "parser.hpp"
#include <iostream>
#include <cstdio>

void OrderbookManager::full_update(Orderbook& book, simdjson::ondemand::object& obj) {
    NanoTime start = now_ns();

    // Parse bids
    book.bid_count = 0;
    book.best_bid = 0;
    book.best_bid_size = 0;

    simdjson::ondemand::array bids;
    if (!obj["bids"].get_array().get(bids)) {
        for (auto level : bids) {
            if (book.bid_count >= MAX_LEVELS) break;
            simdjson::ondemand::object lobj;
            if (level.get_object().get(lobj)) continue;

            std::string_view price_str, size_str;
            if (lobj["price"].get_string().get(price_str)) continue;
            if (lobj["size"].get_string().get(size_str)) continue;

            Price p = parse_price(price_str);
            Size  s = parse_size(size_str);

            book.bids[book.bid_count] = {p, s};
            book.bid_count++;

            if (p > book.best_bid) {
                book.best_bid = p;
                book.best_bid_size = s;
            }
        }
    }

    // Parse asks
    book.ask_count = 0;
    book.best_ask = 9999;
    book.best_ask_size = 0;

    simdjson::ondemand::array asks;
    if (!obj["asks"].get_array().get(asks)) {
        for (auto level : asks) {
            if (book.ask_count >= MAX_LEVELS) break;
            simdjson::ondemand::object lobj;
            if (level.get_object().get(lobj)) continue;

            std::string_view price_str, size_str;
            if (lobj["price"].get_string().get(price_str)) continue;
            if (lobj["size"].get_string().get(size_str)) continue;

            Price p = parse_price(price_str);
            Size  s = parse_size(size_str);

            book.asks[book.ask_count] = {p, s};
            book.ask_count++;

            if (p < book.best_ask) {
                book.best_ask = p;
                book.best_ask_size = s;
            }
        }
    }

    if (book.ask_count == 0) book.best_ask = 0;

    book.local_update_ns = now_ns();
}

void OrderbookManager::print_summary(const Orderbook& book, const char* label) {
    double parse_us = 0;  // caller can compute if needed
    printf("[BOOK] %-3s | bid=%u(%u) ask=%u(%u) spread=%d | levels=%u/%u\n",
           label,
           book.best_bid, book.best_bid_size,
           book.best_ask, book.best_ask_size,
           (int)book.best_ask - (int)book.best_bid,
           book.bid_count, book.ask_count);
}
