#pragma once

#include "types.hpp"
#include <simdjson.h>

class OrderbookManager {
public:
    // Update an orderbook from a parsed book event's JSON object
    // Returns the parse/update latency in nanoseconds
    static void update_from_json(Orderbook& book, simdjson::ondemand::object& obj,
                                 bool is_bids_field);

    // Full update: parse bids and asks arrays from the JSON object into the orderbook
    static void full_update(Orderbook& book, simdjson::ondemand::object& obj);

    // Print orderbook summary
    static void print_summary(const Orderbook& book, const char* label);
};
