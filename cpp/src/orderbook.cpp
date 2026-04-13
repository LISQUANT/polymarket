#include "orderbook.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace {

inline uint32_t advance_epoch(uint32_t epoch, uint32_t (&epoch_by_price)[PRICE_LEVEL_COUNT]) noexcept {
    ++epoch;
    if (epoch == 0) {
        std::memset(epoch_by_price, 0, sizeof(epoch_by_price));
        epoch = 1;
    }
    return epoch;
}

inline Size bid_size_at(const Orderbook& book, Price price) noexcept {
    return (price > 0 && price <= PRICE_ONE && book.bid_epoch_by_price[price] == book.bid_epoch)
        ? book.bid_size_by_price[price]
        : 0;
}

inline Size ask_size_at(const Orderbook& book, Price price) noexcept {
    return (price > 0 && price <= PRICE_ONE && book.ask_epoch_by_price[price] == book.ask_epoch)
        ? book.ask_size_by_price[price]
        : 0;
}

inline void set_bid_level(Orderbook& book, Price price, Size size) noexcept {
    book.bid_size_by_price[price] = size;
    book.bid_epoch_by_price[price] = book.bid_epoch;
}

inline void set_ask_level(Orderbook& book, Price price, Size size) noexcept {
    book.ask_size_by_price[price] = size;
    book.ask_epoch_by_price[price] = book.ask_epoch;
}

inline Price find_best_bid(const Orderbook& book) noexcept {
    for (Price price = PRICE_ONE; price > 0; --price) {
        if (bid_size_at(book, price) > 0) return price;
    }
    return 0;
}

inline Price find_best_ask(const Orderbook& book) noexcept {
    for (Price price = 1; price <= PRICE_ONE; ++price) {
        if (ask_size_at(book, price) > 0) return price;
    }
    return 0;
}

inline void set_best_from_ladders(Orderbook& book) noexcept {
    book.best_bid = find_best_bid(book);
    book.best_ask = find_best_ask(book);
    book.best_bid_size = (book.best_bid > 0) ? bid_size_at(book, book.best_bid) : 0;
    book.best_ask_size = (book.best_ask > 0) ? ask_size_at(book, book.best_ask) : 0;
}

inline void set_best_from_bbo_hints(Orderbook& book, Price hinted_bid, Price hinted_ask) noexcept {
    const Size hinted_bid_size = bid_size_at(book, hinted_bid);
    if (hinted_bid > 0 && hinted_bid_size > 0) {
        book.best_bid = hinted_bid;
        book.best_bid_size = hinted_bid_size;
    } else {
        book.best_bid = find_best_bid(book);
        book.best_bid_size = (book.best_bid > 0) ? bid_size_at(book, book.best_bid) : 0;
    }

    const Size hinted_ask_size = ask_size_at(book, hinted_ask);
    if (hinted_ask > 0 && hinted_ask_size > 0) {
        book.best_ask = hinted_ask;
        book.best_ask_size = hinted_ask_size;
    } else {
        book.best_ask = find_best_ask(book);
        book.best_ask_size = (book.best_ask > 0) ? ask_size_at(book, book.best_ask) : 0;
    }
}

}  // namespace

void OrderbookManager::full_update(Orderbook& book, simdjson::ondemand::object& obj) {
    book.bid_epoch = advance_epoch(book.bid_epoch, book.bid_epoch_by_price);
    book.ask_epoch = advance_epoch(book.ask_epoch, book.ask_epoch_by_price);
    std::fill(std::begin(book.bids), std::end(book.bids), PriceLevel{});
    std::fill(std::begin(book.asks), std::end(book.asks), PriceLevel{});

    // Parse bids
    book.bid_count = 0;

    simdjson::ondemand::array bids;
    if (!obj["bids"].get_array().get(bids)) {
        for (auto level : bids) {
            if (book.bid_count >= MAX_LEVELS) break;
            simdjson::ondemand::object lobj;
            if (level.get_object().get(lobj)) continue;

            std::string_view price_str, size_str;
            if (lobj["price"].get_string().get(price_str)) continue;
            if (lobj["size"].get_string().get(size_str)) continue;

            Price p = 0;
            Size  s = 0;
            if (!try_parse_price(price_str, p) || !try_parse_size(size_str, s) || p == 0 || s == 0) continue;

            book.bids[book.bid_count] = {p, s};
            book.bid_count++;
            set_bid_level(book, p, s);
        }
    }

    // Parse asks
    book.ask_count = 0;

    simdjson::ondemand::array asks;
    if (!obj["asks"].get_array().get(asks)) {
        for (auto level : asks) {
            if (book.ask_count >= MAX_LEVELS) break;
            simdjson::ondemand::object lobj;
            if (level.get_object().get(lobj)) continue;

            std::string_view price_str, size_str;
            if (lobj["price"].get_string().get(price_str)) continue;
            if (lobj["size"].get_string().get(size_str)) continue;

            Price p = 0;
            Size  s = 0;
            if (!try_parse_price(price_str, p) || !try_parse_size(size_str, s) || p == 0 || s == 0) continue;

            book.asks[book.ask_count] = {p, s};
            book.ask_count++;
            set_ask_level(book, p, s);
        }
    }

    set_best_from_ladders(book);
    book.has_snapshot = true;
}

void OrderbookManager::apply_price_change(Orderbook& book, const ParsedPriceChangeEvent& ev) {
    if (ev.price == 0 || ev.price > PRICE_ONE) return;

    if (ev.is_bid) {
        set_bid_level(book, ev.price, ev.size);
    } else {
        set_ask_level(book, ev.price, ev.size);
    }

    set_best_from_ladders(book);
}

void OrderbookManager::apply_best_bid_ask(Orderbook& book, const ParsedBestBidAskEvent& ev) {
    set_best_from_bbo_hints(book, ev.best_bid, ev.best_ask);
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
