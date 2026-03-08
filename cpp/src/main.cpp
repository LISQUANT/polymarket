#include "types.hpp"
#include "websocket.hpp"
#include "parser.hpp"
#include "orderbook.hpp"
#include "arbitrage.hpp"
#include "logger.hpp"

#include <simdjson.h>

#include <iostream>
#include <fstream>
#include <csignal>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <cstdio>

static std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    printf("\n[SIGNAL] Received signal %d, shutting down...\n", sig);
    g_running = false;
}

// Load config from JSON file
Config load_config(const std::string& path) {
    Config config;

    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[CONFIG] Cannot open " << path << ", using defaults\n";
        return config;
    }

    std::string json_str((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(json_str);
    auto doc = parser.iterate(padded);

    std::string_view sv;
    int64_t iv;
    if (!doc["min_edge_threshold_bps"].get_int64().get(iv))
        config.min_edge_threshold_bps = (int)iv;
    if (!doc["taker_fee_bps"].get_int64().get(iv))
        config.taker_fee_bps = (int)iv;
    if (!doc["summary_interval_seconds"].get_int64().get(iv))
        config.summary_interval_seconds = (int)iv;
    if (!doc["ping_interval_seconds"].get_int64().get(iv))
        config.ping_interval_seconds = (int)iv;
    if (!doc["reconnect_max_delay_seconds"].get_int64().get(iv))
        config.reconnect_max_delay_seconds = (int)iv;
    if (!doc["log_file"].get_string().get(sv))
        config.log_file = std::string(sv);

    simdjson::ondemand::array contracts;
    if (!doc["contracts"].get_array().get(contracts)) {
        for (auto elem : contracts) {
            Contract c;
            simdjson::ondemand::object obj;
            if (elem.get_object().get(obj)) continue;

            std::string_view s;
            if (!obj["name"].get_string().get(s)) c.asset_name = std::string(s);
            if (!obj["condition_id"].get_string().get(s)) c.condition_id = std::string(s);
            if (!obj["token_id_yes"].get_string().get(s)) c.token_id_yes = std::string(s);
            if (!obj["token_id_no"].get_string().get(s)) c.token_id_no = std::string(s);

            config.contracts.push_back(std::move(c));
        }
    }

    printf("[CONFIG] Loaded %zu contracts from %s\n", config.contracts.size(), path.c_str());
    return config;
}

int main(int argc, char* argv[]) {
    // Signal handling
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Load config
    std::string config_path = "config.json";
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            config_path = argv[i + 1];
            i++;
        }
    }

    Config config = load_config(config_path);

    if (config.contracts.empty()) {
        std::cerr << "[ERROR] No contracts configured. Add contracts to config.json\n";
        return 1;
    }

    // Build token_id → Contract* lookup map
    std::unordered_map<std::string, Contract*> token_map;
    // Also track which side each token is (YES=true, NO=false)
    std::unordered_map<std::string, bool> token_is_yes;

    for (auto& c : config.contracts) {
        token_map[c.token_id_yes] = &c;
        token_map[c.token_id_no]  = &c;
        token_is_yes[c.token_id_yes] = true;
        token_is_yes[c.token_id_no]  = false;
        printf("[INIT] Contract: %s\n", c.asset_name.c_str());
        printf("       YES token: %.40s...\n", c.token_id_yes.c_str());
        printf("       NO  token: %.40s...\n", c.token_id_no.c_str());
    }

    // Initialize components
    Logger logger(config.log_file);
    MessageParser parser;
    ArbitrageDetector arb(config);
    WebSocketClient ws(config);

    printf("\n[STARTING] Connecting to %s...\n\n", config.websocket_host.c_str());

    if (!ws.connect()) {
        std::cerr << "[FATAL] Could not connect to WebSocket\n";
        return 1;
    }

    auto summary_start = std::chrono::steady_clock::now();
    logger.mark_interval_start();

    // Main message loop
    ws.run([&](const char* data, size_t len, NanoTime t0_recv) {
        if (!g_running) {
            ws.stop();
            return;
        }

        logger.record_message();
        logger.record_msg_size((double)len);

        // Parse and route events — zero-copy from beast buffer
        parser.parse(data, len, t0_recv,
            // Book event callback
            [&](const ParsedBookEvent& ev, simdjson::ondemand::object& obj) {
                NanoTime t1_parse = now_ns();
                logger.record_book_event();

                double parse_us = (t1_parse - t0_recv) / 1000.0;
                logger.record_parse_latency(parse_us);

                // Find which contract and side this belongs to
                std::string asset_id_str(ev.asset_id);
                auto it = token_map.find(asset_id_str);
                if (it == token_map.end()) {
                    printf("[BOOK] Unknown asset_id: %.40s...\n", asset_id_str.c_str());
                    return;
                }

                Contract* contract = it->second;
                bool is_yes = token_is_yes[asset_id_str];

                Orderbook& book = is_yes ? contract->book_yes : contract->book_no;
                OrderbookManager::full_update(book, obj);
                book.timestamp = ev.timestamp;

                NanoTime t2_book = now_ns();
                double book_us = (t2_book - t1_parse) / 1000.0;
                logger.record_book_latency(book_us);

                const char* side = is_yes ? "YES" : "NO ";
                printf("[BOOK] %-24s | %s | bid=%u(%u) ask=%u(%u) spread=%d | parse=%.1fμs book=%.1fμs\n",
                       contract->asset_name.c_str(),
                       side,
                       book.best_bid, book.best_bid_size,
                       book.best_ask, book.best_ask_size,
                       (int)book.best_ask - (int)book.best_bid,
                       parse_us, book_us);

                // Run arbitrage check if we have both sides
                if (contract->book_yes.best_ask > 0 && contract->book_no.best_ask > 0 &&
                    contract->book_yes.best_bid > 0 && contract->book_no.best_bid > 0) {

                    printf("[CONTRACT] %s | YES: %u/%u | NO: %u/%u | sum_asks=%u sum_bids=%u\n",
                           contract->asset_name.c_str(),
                           contract->book_yes.best_bid, contract->book_yes.best_ask,
                           contract->book_no.best_bid, contract->book_no.best_ask,
                           (uint16_t)(contract->book_yes.best_ask + contract->book_no.best_ask),
                           (uint16_t)(contract->book_yes.best_bid + contract->book_no.best_bid));

                    if (arb.check(*contract, t2_book)) {
                        auto& opp = arb.last_opportunity();
                        ArbOpportunity log_opp = opp;
                        log_opp.t0_ws_recv_ns = t0_recv;
                        log_opp.t1_parse_done_ns = t1_parse;
                        log_opp.t4_logged_ns = now_ns();
                        logger.log_opportunity(log_opp);
                    }

                    NanoTime t3_arb = now_ns();
                    double arb_us = (t3_arb - t2_book) / 1000.0;
                    double e2e_us = (t3_arb - t0_recv) / 1000.0;
                    logger.record_arb_latency(arb_us);
                    logger.record_e2e_latency(e2e_us);
                }
            },
            // Trade event callback
            [&](const ParsedTradeEvent& ev) {
                logger.record_trade_event();

                std::string asset_id_str(ev.asset_id);
                auto it = token_map.find(asset_id_str);
                std::string label = "???";
                if (it != token_map.end()) {
                    bool is_yes = token_is_yes[asset_id_str];
                    label = it->second->asset_name + (is_yes ? " YES" : " NO");
                }

                printf("[TRADE] %-25s | price=%.*s side=%.*s size=%.*s\n",
                       label.c_str(),
                       (int)ev.price_str.size(), ev.price_str.data(),
                       (int)ev.side.size(), ev.side.data(),
                       (int)ev.size_str.size(), ev.size_str.data());
            }
        );

        // Periodic summary
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - summary_start).count();
        if (elapsed >= config.summary_interval_seconds) {
            logger.print_summary((int)elapsed, arb.total_checks(),
                                 arb.total_opportunities(), arb.buy_both_count(), arb.sell_both_count(),
                                 arb.cumulative_pnl(), arb.cumulative_volume());
            logger.reset_interval();
            summary_start = now;
        }
    });

    // Final summary
    auto final_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - summary_start).count();
    printf("\n[FINAL]\n");
    logger.print_summary((int)final_elapsed, arb.total_checks(),
                         arb.total_opportunities(), arb.buy_both_count(), arb.sell_both_count());

    return 0;
}
