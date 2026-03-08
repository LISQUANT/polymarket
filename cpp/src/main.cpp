#include "types.hpp"
#include "websocket.hpp"
#include "parser.hpp"
#include "orderbook.hpp"
#include "arbitrage.hpp"
#include "logger.hpp"
#include "runtime.hpp"
#include "pipeline.hpp"

#include <simdjson.h>

#include <iostream>
#include <fstream>
#include <csignal>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <thread>
#include <memory>
#include <cstdio>
#include <cinttypes>

static std::atomic<bool> g_running{true};

struct TokenRoute {
    Contract* contract = nullptr;
    bool is_yes = false;
};

static inline bool has_usable_book(const Orderbook& book) {
    return book.best_bid > 0 && book.best_ask > 0 &&
           book.best_bid_size > 0 && book.best_ask_size > 0;
}

static inline bool contract_ready_for_arb(const Contract& contract) {
    return has_usable_book(contract.book_yes) && has_usable_book(contract.book_no);
}

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
    bool bv;
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
    if (!doc["custom_feature_enabled"].get_bool().get(bv))
        config.custom_feature_enabled = bv;
    if (!doc["initial_dump"].get_bool().get(bv))
        config.initial_dump = bv;
    if (!doc["metrics_enabled"].get_bool().get(bv))
        config.metrics_enabled = bv;
    if (!doc["log_file"].get_string().get(sv))
        config.log_file = std::string(sv);
    if (!doc["hot_path_logging"].get_bool().get(bv))
        config.hot_path_logging = bv;
    if (!doc["flush_csv_each_write"].get_bool().get(bv))
        config.flush_csv_each_write = bv;
    if (!doc["pin_thread_cpu"].get_int64().get(iv))
        config.pin_thread_cpu = (int)iv;
    if (!doc["receiver_cpu"].get_int64().get(iv))
        config.receiver_cpu = (int)iv;
    if (!doc["parser_cpu"].get_int64().get(iv))
        config.parser_cpu = (int)iv;
    if (!doc["logger_cpu"].get_int64().get(iv))
        config.logger_cpu = (int)iv;
    if (!doc["lock_memory"].get_bool().get(bv))
        config.lock_memory = bv;
    if (!doc["prefault_stack_kb"].get_int64().get(iv))
        config.prefault_stack_kb = (size_t)iv;
    if (!doc["realtime_priority"].get_int64().get(iv))
        config.realtime_priority = (int)iv;
    if (!doc["receiver_priority"].get_int64().get(iv))
        config.receiver_priority = (int)iv;
    if (!doc["parser_priority"].get_int64().get(iv))
        config.parser_priority = (int)iv;
    if (!doc["logger_priority"].get_int64().get(iv))
        config.logger_priority = (int)iv;

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
    apply_process_runtime_tuning(config);

    if (config.contracts.empty()) {
        std::cerr << "[ERROR] No contracts configured. Add contracts to config.json\n";
        return 1;
    }

    // Build token_id → Contract* lookup map
    std::unordered_map<std::string_view, TokenRoute,
                       std::hash<std::string_view>, std::equal_to<>> token_routes;
    token_routes.reserve(config.contracts.size() * 2);
    token_routes.max_load_factor(0.7f);

    for (auto& c : config.contracts) {
        token_routes.emplace(c.token_id_yes, TokenRoute{&c, true});
        token_routes.emplace(c.token_id_no, TokenRoute{&c, false});
        printf("[INIT] Contract: %s\n", c.asset_name.c_str());
        printf("       YES token: %.40s...\n", c.token_id_yes.c_str());
        printf("       NO  token: %.40s...\n", c.token_id_no.c_str());
    }

    auto message_queue = std::make_unique<SpscRing<MessageSlot, 128>>();
    auto metrics_queue = std::make_unique<SpscRing<MetricsEvent, 8192>>();
    auto opportunity_queue = std::make_unique<SpscRing<ArbOpportunity, 1024>>();

    std::atomic<bool> receiver_done{false};
    std::atomic<uint64_t> dropped_oversize_messages{0};
    std::atomic<uint64_t> dropped_metrics_events{0};
    std::atomic<uint64_t> dropped_opportunities{0};

    Logger logger(config);

    auto push_metrics = [&](const MetricsEvent& event) {
        if (!config.metrics_enabled) return;
        if (!metrics_queue->try_push_copy(event)) {
            dropped_metrics_events.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto push_opportunity = [&](const ArbOpportunity& opp) {
        if (!opportunity_queue->try_push_copy(opp)) {
            dropped_opportunities.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread logger_thread([&] {
        apply_thread_runtime_tuning(
            "logger",
            config.logger_cpu,
            config.logger_priority > 0 ? config.logger_priority : config.realtime_priority,
            config.prefault_stack_kb);

        while (g_running || !receiver_done.load() || !metrics_queue->empty() || !opportunity_queue->empty()) {
            bool did_work = false;

            MetricsEvent* metric = nullptr;
            while (metrics_queue->front(metric)) {
                did_work = true;
                switch (metric->type) {
                    case MetricsEventType::MESSAGE:
                        logger.consume_message(metric->msg_bytes);
                        break;
                    case MetricsEventType::BOOK:
                        logger.consume_book(metric->parse_us, metric->book_us,
                                            metric->arb_us, metric->e2e_us, metric->arb_checked);
                        break;
                    case MetricsEventType::PRICE_CHANGE:
                        logger.consume_price_change(metric->parse_us, metric->book_us,
                                                    metric->arb_us, metric->e2e_us, metric->arb_checked);
                        break;
                    case MetricsEventType::BEST_BID_ASK:
                        logger.consume_bbo(metric->parse_us, metric->book_us,
                                           metric->arb_us, metric->e2e_us, metric->arb_checked);
                        break;
                    case MetricsEventType::TRADE:
                        logger.consume_trade();
                        break;
                }
                metrics_queue->pop();
            }

            ArbOpportunity* opp = nullptr;
            while (opportunity_queue->front(opp)) {
                did_work = true;
                logger.consume_opportunity(*opp);
                opportunity_queue->pop();
            }

            logger.maybe_print_summary();

            if (!did_work) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        std::printf("\n[FINAL]\n");
        logger.print_final_summary();
    });

    std::thread receiver_thread([&] {
        apply_thread_runtime_tuning(
            "receiver",
            config.receiver_cpu,
            config.receiver_priority > 0 ? config.receiver_priority : config.realtime_priority,
            config.prefault_stack_kb);

        WebSocketClient ws(config);

        std::printf("\n[STARTING] Connecting to %s...\n\n", config.websocket_host.c_str());
        if (!ws.connect()) {
            std::fprintf(stderr, "[FATAL] Could not connect to WebSocket\n");
            g_running = false;
            receiver_done = true;
            return;
        }

        ws.run([&](const char* data, size_t len, NanoTime t0_recv) {
            if (!g_running) {
                ws.stop();
                return;
            }

            if (len > MessageParser::kBufferCapacity) {
                dropped_oversize_messages.fetch_add(1, std::memory_order_relaxed);
                if (config.hot_path_logging) {
                    std::fprintf(stderr, "[DROP] Oversize message: %zu bytes\n", len);
                }
                return;
            }

            while (g_running &&
                   !message_queue->try_push([&](MessageSlot& slot) {
                       store_message_slot(slot, data, len, t0_recv);
                   })) {
                cpu_relax();
            }
        });

        receiver_done = true;
    });

    apply_thread_runtime_tuning(
        "parser",
        config.parser_cpu >= 0 ? config.parser_cpu : config.pin_thread_cpu,
        config.parser_priority > 0 ? config.parser_priority : config.realtime_priority,
        config.prefault_stack_kb);

    MessageParser parser;
    ArbitrageDetector arb(config);

    auto finalize_contract_update = [&](Contract& contract, NanoTime t0_recv, NanoTime t1_parse,
                                        NanoTime t2_book) {
        struct ArbResult {
            bool checked = false;
            float arb_us = 0.0f;
            float e2e_us = 0.0f;
        } result;

        if (!contract_ready_for_arb(contract)) {
            return result;
        }

        if (config.hot_path_logging) {
            std::printf("[CONTRACT] %s | YES: %u/%u | NO: %u/%u | sum_asks=%u sum_bids=%u\n",
                        contract.asset_name.c_str(),
                        contract.book_yes.best_bid, contract.book_yes.best_ask,
                        contract.book_no.best_bid, contract.book_no.best_ask,
                        static_cast<uint16_t>(contract.book_yes.best_ask + contract.book_no.best_ask),
                        static_cast<uint16_t>(contract.book_yes.best_bid + contract.book_no.best_bid));
        }

        if (arb.check(contract, t2_book)) {
            auto log_opp = arb.last_opportunity();
            log_opp.t0_ws_recv_ns = t0_recv;
            log_opp.t1_parse_done_ns = config.metrics_enabled ? t1_parse : t0_recv;
            log_opp.t2_book_updated_ns = config.metrics_enabled ? t2_book : t0_recv;
            log_opp.t3_arb_checked_ns = config.metrics_enabled ? log_opp.t3_arb_checked_ns : t0_recv;
            log_opp.t4_logged_ns = config.metrics_enabled ? now_ns() : t0_recv;
            push_opportunity(log_opp);
        }

        if (config.metrics_enabled) {
            const NanoTime t3_arb = now_ns();
            result.checked = true;
            result.arb_us = static_cast<float>((t3_arb - t2_book) / 1000.0);
            result.e2e_us = static_cast<float>((t3_arb - t0_recv) / 1000.0);
        }

        return result;
    };

    while (g_running || !receiver_done.load() || !message_queue->empty()) {
        MessageSlot* slot = nullptr;
        if (!message_queue->front(slot)) {
            if (receiver_done.load()) {
                break;
            }
            cpu_relax();
            continue;
        }

        if (config.metrics_enabled) {
            push_metrics(MetricsEvent{
                MetricsEventType::MESSAGE,
                static_cast<uint32_t>(slot->len),
                0.0f, 0.0f, 0.0f, 0.0f, false
            });
        }

        parser.parse_padded(slot->data, slot->len,
            MessageParser::kBufferCapacity + simdjson::SIMDJSON_PADDING,
            [&](const ParsedBookEvent& ev, simdjson::ondemand::object& obj) {
                const NanoTime t1_parse = config.metrics_enabled ? now_ns() : 0;
                float parse_us = 0.0f;
                if (config.metrics_enabled) {
                    parse_us = static_cast<float>((t1_parse - slot->recv_time) / 1000.0);
                }

                auto it = token_routes.find(ev.asset_id);
                if (it == token_routes.end()) {
                    std::printf("[BOOK] Unknown asset_id: %.*s...\n",
                                (int)std::min<size_t>(ev.asset_id.size(), 40), ev.asset_id.data());
                    return;
                }

                Contract* contract = it->second.contract;
                Orderbook& book = it->second.is_yes ? contract->book_yes : contract->book_no;
                OrderbookManager::full_update(book, obj);
                book.timestamp = ev.timestamp;

                const NanoTime t2_book = config.metrics_enabled ? now_ns() : 0;
                book.local_update_ns = t2_book;

                float book_us = 0.0f;
                if (config.metrics_enabled) {
                    book_us = static_cast<float>((t2_book - t1_parse) / 1000.0);
                }

                const auto arb_result = finalize_contract_update(*contract, slot->recv_time, t1_parse, t2_book);
                push_metrics(MetricsEvent{
                    MetricsEventType::BOOK,
                    0,
                    parse_us,
                    book_us,
                    arb_result.arb_us,
                    arb_result.e2e_us,
                    arb_result.checked
                });
            },
            [&](const ParsedTradeEvent& ev) {
                if (config.hot_path_logging) {
                    auto it = token_routes.find(ev.asset_id);
                    std::string label = "???";
                    if (it != token_routes.end()) {
                        label = it->second.contract->asset_name + (it->second.is_yes ? " YES" : " NO");
                    }

                    std::printf("[TRADE] %-25s | price=%.*s side=%.*s size=%.*s\n",
                                label.c_str(),
                                (int)ev.price_str.size(), ev.price_str.data(),
                                (int)ev.side.size(), ev.side.data(),
                                (int)ev.size_str.size(), ev.size_str.data());
                }

                push_metrics(MetricsEvent{
                    MetricsEventType::TRADE,
                    0,
                    0.0f, 0.0f, 0.0f, 0.0f, false
                });
            },
            [&](const ParsedPriceChangeEvent& ev) {
                const NanoTime t1_parse = config.metrics_enabled ? now_ns() : 0;
                float parse_us = 0.0f;
                if (config.metrics_enabled) {
                    parse_us = static_cast<float>((t1_parse - slot->recv_time) / 1000.0);
                }

                auto it = token_routes.find(ev.asset_id);
                if (it == token_routes.end()) {
                    if (config.hot_path_logging) {
                        std::printf("[PXCHG] Unknown asset_id: %.*s...\n",
                                    (int)std::min<size_t>(ev.asset_id.size(), 40), ev.asset_id.data());
                    }
                    return;
                }

                Contract* contract = it->second.contract;
                Orderbook& book = it->second.is_yes ? contract->book_yes : contract->book_no;
                OrderbookManager::apply_price_change(book, ev);
                book.timestamp = ev.timestamp;

                const NanoTime t2_book = config.metrics_enabled ? now_ns() : 0;
                book.local_update_ns = t2_book;

                float book_us = 0.0f;
                if (config.metrics_enabled) {
                    book_us = static_cast<float>((t2_book - t1_parse) / 1000.0);
                }

                const auto arb_result = finalize_contract_update(*contract, slot->recv_time, t1_parse, t2_book);
                push_metrics(MetricsEvent{
                    MetricsEventType::PRICE_CHANGE,
                    0,
                    parse_us,
                    book_us,
                    arb_result.arb_us,
                    arb_result.e2e_us,
                    arb_result.checked
                });
            },
            [&](const ParsedBestBidAskEvent& ev) {
                const NanoTime t1_parse = config.metrics_enabled ? now_ns() : 0;
                float parse_us = 0.0f;
                if (config.metrics_enabled) {
                    parse_us = static_cast<float>((t1_parse - slot->recv_time) / 1000.0);
                }

                auto it = token_routes.find(ev.asset_id);
                if (it == token_routes.end()) {
                    if (config.hot_path_logging) {
                        std::printf("[BBO] Unknown asset_id: %.*s...\n",
                                    (int)std::min<size_t>(ev.asset_id.size(), 40), ev.asset_id.data());
                    }
                    return;
                }

                Contract* contract = it->second.contract;
                Orderbook& book = it->second.is_yes ? contract->book_yes : contract->book_no;
                OrderbookManager::apply_best_bid_ask(book, ev);
                book.timestamp = ev.timestamp;

                const NanoTime t2_book = config.metrics_enabled ? now_ns() : 0;
                book.local_update_ns = t2_book;

                float book_us = 0.0f;
                if (config.metrics_enabled) {
                    book_us = static_cast<float>((t2_book - t1_parse) / 1000.0);
                }

                const auto arb_result = finalize_contract_update(*contract, slot->recv_time, t1_parse, t2_book);
                push_metrics(MetricsEvent{
                    MetricsEventType::BEST_BID_ASK,
                    0,
                    parse_us,
                    book_us,
                    arb_result.arb_us,
                    arb_result.e2e_us,
                    arb_result.checked
                });
            });

        message_queue->pop();
    }

    g_running = false;

    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }
    if (logger_thread.joinable()) {
        logger_thread.join();
    }

    const uint64_t oversize_drops = dropped_oversize_messages.load(std::memory_order_relaxed);
    const uint64_t metrics_drops = dropped_metrics_events.load(std::memory_order_relaxed);
    const uint64_t opportunity_drops = dropped_opportunities.load(std::memory_order_relaxed);
    if (oversize_drops || metrics_drops || opportunity_drops) {
        std::printf("[PIPELINE] Dropped oversized messages=%" PRIu64
                    " | dropped metrics=%" PRIu64
                    " | dropped opportunities=%" PRIu64 "\n",
                    oversize_drops, metrics_drops, opportunity_drops);
    }

    return 0;
}
