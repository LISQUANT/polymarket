#include "types.hpp"
#include "websocket.hpp"
#include "parser.hpp"
#include "orderbook.hpp"
#include "arbitrage.hpp"
#include "logger.hpp"
#include "market_metadata.hpp"
#include "runtime.hpp"
#include "pipeline.hpp"

#include <simdjson.h>

#include <iostream>
#include <fstream>
#include <csignal>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <thread>
#include <memory>
#include <cstdio>
#include <cinttypes>
#include <unistd.h>

static std::atomic<bool> g_running{true};

struct TokenRoute {
    Contract* contract = nullptr;
    bool is_yes = false;
};

static inline bool has_usable_book(const Orderbook& book) {
    return book.has_snapshot && book.best_bid > 0 && book.best_ask > 0;
}

static inline bool has_usable_yes_book(const Contract& contract) {
    return contract.book_yes.has_snapshot &&
           contract.book_yes.best_bid > 0 &&
           contract.book_yes.best_ask > 0;
}

static inline bool contract_ready_for_arb(const Contract& contract) {
    return has_usable_book(contract.book_yes) && has_usable_book(contract.book_no);
}

static inline bool group_ready_for_arb(const ContractGroup& group) {
    if (!group.exhaustive || group.contracts.size() < 2) return false;
    for (const Contract* contract : group.contracts) {
        if (!has_usable_yes_book(*contract)) {
            return false;
        }
    }
    return true;
}

template <typename T>
void update_atomic_max(std::atomic<T>& target, T value) {
    T current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void signal_handler(int sig) {
    g_running.store(false, std::memory_order_relaxed);
    constexpr char kSignalMsg[] = "\n[SIGNAL] Shutdown requested\n";
    (void)sig;
    (void)!::write(STDERR_FILENO, kSignalMsg, sizeof(kSignalMsg) - 1);
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
    if (!doc["warmup_seconds"].get_int64().get(iv))
        config.warmup_seconds = (int)iv;
    if (!doc["ping_interval_seconds"].get_int64().get(iv))
        config.ping_interval_seconds = (int)iv;
    if (!doc["stale_feed_timeout_seconds"].get_int64().get(iv))
        config.stale_feed_timeout_seconds = (int)iv;
    if (!doc["reconnect_max_delay_seconds"].get_int64().get(iv))
        config.reconnect_max_delay_seconds = (int)iv;
    if (!doc["message_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.message_queue_capacity = (size_t)iv;
    if (!doc["metrics_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.metrics_queue_capacity = (size_t)iv;
    if (!doc["opportunity_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.opportunity_queue_capacity = (size_t)iv;
    if (!doc["replay_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.replay_queue_capacity = (size_t)iv;
    if (!doc["active_market_report_limit"].get_int64().get(iv) && iv > 0)
        config.active_market_report_limit = (size_t)iv;
    if (!doc["custom_feature_enabled"].get_bool().get(bv))
        config.custom_feature_enabled = bv;
    if (!doc["initial_dump"].get_bool().get(bv))
        config.initial_dump = bv;
    if (!doc["metrics_enabled"].get_bool().get(bv))
        config.metrics_enabled = bv;
    if (!doc["edge_telemetry_enabled"].get_bool().get(bv))
        config.edge_telemetry_enabled = bv;
    if (!doc["log_file"].get_string().get(sv))
        config.log_file = std::string(sv);
    if (!doc["near_miss_log_file"].get_string().get(sv))
        config.near_miss_log_file = std::string(sv);
    if (!doc["replay_log_file"].get_string().get(sv))
        config.replay_log_file = std::string(sv);
    if (!doc["replay_logging_enabled"].get_bool().get(bv))
        config.replay_logging_enabled = bv;
    if (!doc["hot_path_logging"].get_bool().get(bv))
        config.hot_path_logging = bv;
    if (!doc["flush_csv_each_write"].get_bool().get(bv))
        config.flush_csv_each_write = bv;
    if (!doc["fetch_market_metadata"].get_bool().get(bv))
        config.fetch_market_metadata = bv;
    if (!doc["enable_group_arbitrage"].get_bool().get(bv))
        config.enable_group_arbitrage = bv;
    if (!doc["auto_detect_exhaustive_groups"].get_bool().get(bv))
        config.auto_detect_exhaustive_groups = bv;
    if (!doc["maker_arb_enabled"].get_bool().get(bv))
        config.maker_arb_enabled = bv;
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

    simdjson::ondemand::array groups;
    if (!doc["groups"].get_array().get(groups)) {
        for (auto elem : groups) {
            Config::ConfiguredGroup group;
            simdjson::ondemand::object obj;
            if (elem.get_object().get(obj)) continue;

            std::string_view s;
            if (!obj["key"].get_string().get(s)) group.key = std::string(s);
            if (!obj["display_name"].get_string().get(s)) group.display_name = std::string(s);
            if (!obj["exhaustive"].get_bool().get(bv)) group.exhaustive = bv;

            simdjson::ondemand::array condition_ids;
            if (!obj["condition_ids"].get_array().get(condition_ids)) {
                for (auto condition_elem : condition_ids) {
                    if (!condition_elem.get_string().get(s)) {
                        group.condition_ids.emplace_back(s);
                    }
                }
            }

            if (group.display_name.empty()) {
                group.display_name = group.key;
            }
            if (!group.condition_ids.empty()) {
                config.configured_groups.push_back(std::move(group));
            }
        }
    }

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
            if (!obj["taker_fee_bps_override"].get_int64().get(iv)) c.taker_fee_bps_override = (int)iv;

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

    std::vector<ContractGroup> contract_groups;
    std::unordered_map<Contract*, std::vector<ContractGroup*>> contract_group_routes;
    std::unordered_map<std::string_view, Contract*> contracts_by_condition_id;
    contracts_by_condition_id.reserve(config.contracts.size());
    for (auto& contract : config.contracts) {
        contracts_by_condition_id.emplace(contract.condition_id, &contract);
    }
    std::unordered_set<std::string> seen_group_keys;

    if (config.enable_group_arbitrage) {
        contract_groups.reserve(config.configured_groups.size());
        for (const auto& configured_group : config.configured_groups) {
            ContractGroup group;
            group.key = configured_group.key.empty()
                ? configured_group.display_name
                : configured_group.key;
            group.display_name = configured_group.display_name.empty()
                ? group.key
                : configured_group.display_name;
            group.exhaustive = configured_group.exhaustive;

            bool missing_contract = false;
            for (const std::string& condition_id : configured_group.condition_ids) {
                const auto it = contracts_by_condition_id.find(condition_id);
                if (it == contracts_by_condition_id.end()) {
                    std::fprintf(stderr, "[GROUP] %s | missing contract condition_id=%s\n",
                                 group.display_name.c_str(), condition_id.c_str());
                    missing_contract = true;
                    break;
                }
                group.contracts.push_back(it->second);
            }

            if (missing_contract || group.contracts.size() < 2 || !group.exhaustive) {
                continue;
            }
            if (!seen_group_keys.emplace(group.key).second) {
                continue;
            }
            contract_groups.push_back(std::move(group));
        }
    }

    if (config.fetch_market_metadata) {
        MarketMetadataClient metadata_client;

        for (auto& c : config.contracts) {
            MarketMetadata metadata;
            std::string error;
            if (!metadata_client.fetch_market_by_token(c.token_id_yes, metadata, error)) {
                std::fprintf(stderr, "[META] %s | market lookup failed: %s\n",
                             c.asset_name.c_str(), error.c_str());
                continue;
            }

            c.fee_schedule_enabled = metadata.fees_enabled;
            c.fee_rate = metadata.fee_rate;
            c.fee_exponent = metadata.fee_exponent;
            c.base_fee_bps = metadata.base_fee_bps;
            c.neg_risk = metadata.neg_risk;
            c.market_metadata_loaded = true;
            c.event_slug = std::move(metadata.event_slug);
            c.event_title = std::move(metadata.event_title);

            std::printf("[META] %s | event=%s | negRisk=%s | fees=%s",
                        c.asset_name.c_str(),
                        c.event_slug.empty() ? "n/a" : c.event_slug.c_str(),
                        c.neg_risk ? "true" : "false",
                        c.fee_schedule_enabled ? "dynamic" : (c.market_metadata_loaded ? "none" : "fallback"));
            if (c.fee_schedule_enabled) {
                std::printf(" | rate=%.4f exp=%d", c.fee_rate, c.fee_exponent);
            } else if (c.market_metadata_loaded) {
                std::printf(" | rate=0");
            } else if (c.taker_fee_bps_override >= 0) {
                std::printf(" | override=%d bps", c.taker_fee_bps_override);
            } else {
                std::printf(" | default=%d bps", config.taker_fee_bps);
            }
            std::printf("\n");
        }

        if (config.enable_group_arbitrage && config.auto_detect_exhaustive_groups) {
            std::unordered_map<std::string, std::vector<Contract*>> contracts_by_event;
            for (auto& c : config.contracts) {
                if (c.neg_risk && !c.event_slug.empty()) {
                    contracts_by_event[c.event_slug].push_back(&c);
                }
            }

            contract_groups.reserve(contracts_by_event.size());
            for (const auto& [event_slug, members] : contracts_by_event) {
                if (members.size() < 2) continue;

                size_t event_market_count = 0;
                std::string error;
                if (!metadata_client.fetch_event_market_count(event_slug, event_market_count, error)) {
                    std::fprintf(stderr, "[GROUP] %s | event lookup failed: %s\n",
                                 event_slug.c_str(), error.c_str());
                    continue;
                }
                if (event_market_count != members.size()) {
                    continue;
                }

                ContractGroup group;
                group.key = event_slug;
                group.display_name = members.front()->event_title.empty()
                    ? event_slug
                    : members.front()->event_title;
                group.exhaustive = true;
                group.contracts = members;
                if (!seen_group_keys.emplace(group.key).second) {
                    continue;
                }
                contract_groups.push_back(std::move(group));
            }
        }
    }

    if (config.enable_group_arbitrage) {
        for (auto& group : contract_groups) {
            std::printf("[GROUP] Enabled exhaustive basket: %s | legs=%zu\n",
                        group.display_name.c_str(), group.contracts.size());
            for (Contract* contract : group.contracts) {
                contract_group_routes[contract].push_back(&group);
            }
        }
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

    auto message_queue = std::make_unique<SpscRing<MessageSlot>>(config.message_queue_capacity);
    auto metrics_queue = std::make_unique<SpscRing<MetricsEvent>>(config.metrics_queue_capacity);
    auto opportunity_queue = std::make_unique<SpscRing<ArbOpportunity>>(config.opportunity_queue_capacity);
    auto replay_queue = std::make_unique<SpscRing<ReplaySnapshot>>(config.replay_queue_capacity);

    std::atomic<bool> receiver_done{false};
    std::atomic<uint64_t> dropped_oversize_messages{0};
    std::atomic<uint64_t> dropped_metrics_events{0};
    std::atomic<uint64_t> dropped_opportunities{0};
    std::atomic<uint64_t> dropped_replay_snapshots{0};
    std::atomic<uint64_t> message_queue_backpressure_events{0};
    std::atomic<uint64_t> message_queue_backpressure_spins{0};
    std::atomic<uint64_t> max_message_bytes_seen{0};
    std::atomic<uint64_t> max_frame_opportunities_seen{0};
    WebSocketClientStats websocket_stats{};

    auto logger = std::make_unique<Logger>(config);

    auto push_metrics = [&](const MetricsEvent& event) {
        if (event.type == MetricsEventType::EDGE_SAMPLE) {
            if (!config.edge_telemetry_enabled) return;
        } else if (!config.metrics_enabled) {
            return;
        }
        if (!metrics_queue->try_push_copy(event)) {
            dropped_metrics_events.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto push_opportunity = [&](const ArbOpportunity& opp) {
        if (!opportunity_queue->try_push_copy(opp)) {
            dropped_opportunities.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto push_replay_snapshot = [&](const ReplaySnapshot& snapshot) {
        if (!config.replay_logging_enabled) return;
        if (!replay_queue->try_push_copy(snapshot)) {
            dropped_replay_snapshots.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread logger_thread([&] {
        apply_thread_runtime_tuning(
            "logger",
            config.logger_cpu,
            config.logger_priority > 0 ? config.logger_priority : config.realtime_priority,
            config.prefault_stack_kb);

        while (g_running || !receiver_done.load() || !metrics_queue->empty() ||
               !opportunity_queue->empty() || !replay_queue->empty()) {
            bool did_work = false;

            MetricsEvent* metric = nullptr;
            while (metrics_queue->front(metric)) {
                did_work = true;
                switch (metric->type) {
                    case MetricsEventType::MESSAGE:
                        logger->consume_message(metric->msg_bytes);
                        break;
                    case MetricsEventType::BOOK:
                        logger->consume_book(metric->event_count, metric->has_latency_sample,
                                             metric->queue_us, metric->parse_us, metric->book_us,
                                             metric->arb_us, metric->e2e_us, metric->arb_checks);
                        break;
                    case MetricsEventType::PRICE_CHANGE:
                        logger->consume_price_change(metric->event_count, metric->has_latency_sample,
                                                     metric->queue_us, metric->parse_us, metric->book_us,
                                                     metric->arb_us, metric->e2e_us, metric->arb_checks);
                        break;
                    case MetricsEventType::BEST_BID_ASK:
                        logger->consume_bbo(metric->event_count, metric->has_latency_sample,
                                            metric->queue_us, metric->parse_us, metric->book_us,
                                            metric->arb_us, metric->e2e_us, metric->arb_checks);
                        break;
                    case MetricsEventType::TRADE:
                        logger->consume_trade(metric->event_count);
                        break;
                    case MetricsEventType::EDGE_SAMPLE:
                        logger->consume_edge_sample(metric->sample_time_ns,
                                                    metric->edge_label,
                                                    metric->arb_kind,
                                                    metric->reference_value,
                                                    metric->leg_count,
                                                    metric->raw_edge_bps,
                                                    metric->net_edge_bps);
                        break;
                }
                metrics_queue->pop();
            }

            ArbOpportunity* opp = nullptr;
            while (opportunity_queue->front(opp)) {
                did_work = true;
                logger->consume_opportunity(*opp);
                opportunity_queue->pop();
            }

            ReplaySnapshot* snapshot = nullptr;
            while (replay_queue->front(snapshot)) {
                did_work = true;
                logger->consume_replay_snapshot(*snapshot);
                replay_queue->pop();
            }

            logger->flush_pending_csv();
            logger->maybe_print_summary();

            if (!did_work) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        std::printf("\n[FINAL]\n");
        logger->print_final_summary();
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
            websocket_stats = ws.stats();
            g_running = false;
            receiver_done = true;
            return;
        }

        ws.run([&](const char* data, size_t len, NanoTime t0_recv) {
            update_atomic_max(max_message_bytes_seen, static_cast<uint64_t>(len));
            if (len > MessageParser::kBufferCapacity) {
                dropped_oversize_messages.fetch_add(1, std::memory_order_relaxed);
                if (config.hot_path_logging) {
                    std::fprintf(stderr, "[DROP] Oversize message: %zu bytes\n", len);
                }
                return;
            }

            bool waited_for_slot = false;
            while (g_running &&
                   !message_queue->try_push([&](MessageSlot& slot) {
                       store_message_slot(slot, data, len, t0_recv);
                   })) {
                waited_for_slot = true;
                message_queue_backpressure_spins.fetch_add(1, std::memory_order_relaxed);
                cpu_relax();
            }
            if (waited_for_slot) {
                message_queue_backpressure_events.fetch_add(1, std::memory_order_relaxed);
            }
        }, [&] {
            return !g_running.load(std::memory_order_relaxed);
        });

        websocket_stats = ws.stats();
        receiver_done = true;
    });

    apply_thread_runtime_tuning(
        "parser",
        config.parser_cpu >= 0 ? config.parser_cpu : config.pin_thread_cpu,
        config.parser_priority > 0 ? config.parser_priority : config.realtime_priority,
        config.prefault_stack_kb);

    auto parser = std::make_unique<MessageParser>();
    ArbitrageDetector arb(config);
    uint64_t next_frame_id = 1;
    std::vector<ArbOpportunity> frame_opportunities;
    frame_opportunities.reserve(
        std::max<size_t>(ArbCheckOutput::kMaxOpportunities,
                         config.contracts.size() * ArbCheckOutput::kMaxOpportunities));
    std::vector<Contract*> touched_contracts;
    touched_contracts.reserve(config.contracts.size());
    std::vector<ContractGroup*> touched_groups;
    touched_groups.reserve(contract_groups.size());
    std::vector<Orderbook*> touched_books;
    touched_books.reserve(config.contracts.size() * 2);

    auto collect_output = [&](const ArbCheckOutput& output, NanoTime t0_recv,
                              NanoTime t1_parse_done, NanoTime t2_books_done) -> uint16_t {
        for (size_t i = 0; i < output.edge_sample_count; ++i) {
            const EdgeTelemetrySample& sample = output.edge_samples[i];
            if (!sample.valid) continue;

            MetricsEvent edge_event{};
            edge_event.type = MetricsEventType::EDGE_SAMPLE;
            edge_event.sample_time_ns = sample.sample_time_ns;
            edge_event.edge_label = sample.label;
            edge_event.arb_kind = sample.arb_kind;
            edge_event.reference_value = sample.reference_value;
            edge_event.leg_count = sample.leg_count;
            edge_event.raw_edge_bps = sample.raw_edge_bps;
            edge_event.net_edge_bps = sample.net_edge_bps;
            push_metrics(edge_event);
        }

        for (size_t i = 0; i < output.opportunity_count; ++i) {
            auto log_opp = output.opportunities[i];
            log_opp.t0_ws_recv_ns = t0_recv;
            log_opp.t1_parse_done_ns = config.metrics_enabled ? t1_parse_done : t0_recv;
            log_opp.t2_book_updated_ns = config.metrics_enabled ? t2_books_done : t0_recv;
            log_opp.t3_arb_checked_ns = config.metrics_enabled ? log_opp.t3_arb_checked_ns : t0_recv;
            log_opp.t4_logged_ns = config.metrics_enabled ? now_ns() : t0_recv;
            log_opp.paper_trade_pnl = 0.0;
            log_opp.counts_toward_paper_trade = false;
            frame_opportunities.push_back(log_opp);
        }

        return output.checks_performed;
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
            MetricsEvent msg_event{};
            msg_event.type = MetricsEventType::MESSAGE;
            msg_event.msg_bytes = static_cast<uint32_t>(slot->len);
            push_metrics(msg_event);
        }

        struct FrameStats {
            uint16_t book_events = 0;
            uint16_t price_change_events = 0;
            uint16_t bbo_events = 0;
            uint16_t trade_events = 0;
            uint64_t book_apply_ns = 0;
        } frame{};
        frame_opportunities.clear();
        touched_contracts.clear();
        touched_groups.clear();
        touched_books.clear();

        const uint64_t frame_id = next_frame_id++;
        const NanoTime t1_parse_start = config.metrics_enabled ? now_ns() : 0;
        const float queue_us = config.metrics_enabled
            ? static_cast<float>((t1_parse_start - slot->recv_time) / 1000.0)
            : 0.0f;

        auto mark_contract_touched = [&](Contract& contract) {
            if (contract.touched_frame_id != frame_id) {
                contract.touched_frame_id = frame_id;
                touched_contracts.push_back(&contract);
            }

            if (!config.enable_group_arbitrage) return;
            const auto group_it = contract_group_routes.find(&contract);
            if (group_it == contract_group_routes.end()) return;
            for (ContractGroup* group : group_it->second) {
                if (group->touched_frame_id != frame_id) {
                    group->touched_frame_id = frame_id;
                    touched_groups.push_back(group);
                }
            }
        };

        auto mark_book_touched = [&](Orderbook& book) {
            if (book.touched_frame_id != frame_id) {
                book.touched_frame_id = frame_id;
                touched_books.push_back(&book);
            }
        };

        auto apply_update = [&](Contract& contract, Orderbook& book, uint64_t timestamp, auto&& update_fn) {
            const NanoTime t_book_start = config.metrics_enabled ? now_ns() : 0;
            update_fn(book);
            book.timestamp = timestamp;
            ++contract.total_updates;
            if (&book == &contract.book_yes) {
                ++contract.yes_updates;
            } else {
                ++contract.no_updates;
            }
            if (config.metrics_enabled) {
                frame.book_apply_ns += now_ns() - t_book_start;
            }
            mark_book_touched(book);
            mark_contract_touched(contract);
        };

        parser->parse_padded(slot->data, slot->len,
            MessageParser::kBufferCapacity + simdjson::SIMDJSON_PADDING,
            [&](const ParsedBookEvent& ev, simdjson::ondemand::object& obj) {
                auto it = token_routes.find(ev.asset_id);
                if (it == token_routes.end()) {
                    std::printf("[BOOK] Unknown asset_id: %.*s...\n",
                                (int)std::min<size_t>(ev.asset_id.size(), 40), ev.asset_id.data());
                    return;
                }

                ++frame.book_events;
                Contract& contract = *it->second.contract;
                Orderbook& book = it->second.is_yes ? contract.book_yes : contract.book_no;
                apply_update(contract, book, ev.timestamp, [&](Orderbook& target) {
                    OrderbookManager::full_update(target, obj);
                });
            },
            [&](const ParsedTradeEvent& ev) {
                ++frame.trade_events;
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
            },
            [&](const ParsedPriceChangeEvent& ev) {
                auto it = token_routes.find(ev.asset_id);
                if (it == token_routes.end()) {
                    if (config.hot_path_logging) {
                        std::printf("[PXCHG] Unknown asset_id: %.*s...\n",
                                    (int)std::min<size_t>(ev.asset_id.size(), 40), ev.asset_id.data());
                    }
                    return;
                }

                ++frame.price_change_events;
                Contract& contract = *it->second.contract;
                Orderbook& book = it->second.is_yes ? contract.book_yes : contract.book_no;
                apply_update(contract, book, ev.timestamp, [&](Orderbook& target) {
                    OrderbookManager::apply_price_change(target, ev);
                });
            },
            [&](const ParsedBestBidAskEvent& ev) {
                auto it = token_routes.find(ev.asset_id);
                if (it == token_routes.end()) {
                    if (config.hot_path_logging) {
                        std::printf("[BBO] Unknown asset_id: %.*s...\n",
                                    (int)std::min<size_t>(ev.asset_id.size(), 40), ev.asset_id.data());
                    }
                    return;
                }

                ++frame.bbo_events;
                Contract& contract = *it->second.contract;
                Orderbook& book = it->second.is_yes ? contract.book_yes : contract.book_no;
                apply_update(contract, book, ev.timestamp, [&](Orderbook& target) {
                    OrderbookManager::apply_best_bid_ask(target, ev);
                });
            });

        const NanoTime t2_parse_done = config.metrics_enabled ? now_ns() : 0;
        const NanoTime t3_books_done = config.metrics_enabled ? now_ns() : 0;

        for (Orderbook* book : touched_books) {
            book->local_update_ns = t3_books_done;
        }

        uint16_t arb_checks = 0;
        for (Contract* contract : touched_contracts) {
            if (!contract_ready_for_arb(*contract)) {
                continue;
            }

            if (config.hot_path_logging) {
                std::printf("[CONTRACT] %s | YES: %u/%u | NO: %u/%u | sum_asks=%u sum_bids=%u\n",
                            contract->asset_name.c_str(),
                            contract->book_yes.best_bid, contract->book_yes.best_ask,
                            contract->book_no.best_bid, contract->book_no.best_ask,
                            static_cast<uint16_t>(contract->book_yes.best_ask + contract->book_no.best_ask),
                            static_cast<uint16_t>(contract->book_yes.best_bid + contract->book_no.best_bid));
            }

            arb_checks = static_cast<uint16_t>(
                arb_checks + collect_output(arb.check(*contract, t3_books_done),
                                            slot->recv_time, t2_parse_done, t3_books_done));
        }

        if (config.enable_group_arbitrage) {
            for (ContractGroup* group : touched_groups) {
                if (!group_ready_for_arb(*group)) {
                    continue;
                }
                arb_checks = static_cast<uint16_t>(
                    arb_checks + collect_output(arb.check_group(*group, t3_books_done),
                                                slot->recv_time, t2_parse_done, t3_books_done));
            }
        }

        size_t best_paper_trade_index = frame_opportunities.size();
        auto is_better_paper_trade = [](const ArbOpportunity& lhs, const ArbOpportunity& rhs) {
            if (lhs.theoretical_pnl != rhs.theoretical_pnl) {
                return lhs.theoretical_pnl < rhs.theoretical_pnl;
            }
            if (lhs.edge_bps != rhs.edge_bps) {
                return lhs.edge_bps < rhs.edge_bps;
            }
            if (lhs.size_shares != rhs.size_shares) {
                return lhs.size_shares < rhs.size_shares;
            }
            return lhs.leg_count < rhs.leg_count;
        };

        for (size_t i = 0; i < frame_opportunities.size(); ++i) {
            const ArbOpportunity& opp = frame_opportunities[i];
            if (is_maker_arb_kind(opp.arb_kind)) {
                continue;
            }
            if (opp.theoretical_pnl <= 0.0) {
                continue;
            }
            if (best_paper_trade_index == frame_opportunities.size() ||
                is_better_paper_trade(frame_opportunities[best_paper_trade_index], opp)) {
                best_paper_trade_index = i;
            }
        }

        if (best_paper_trade_index < frame_opportunities.size()) {
            ArbOpportunity& best_opp = frame_opportunities[best_paper_trade_index];
            best_opp.counts_toward_paper_trade = true;
            best_opp.paper_trade_pnl = best_opp.theoretical_pnl;
        }
        update_atomic_max(max_frame_opportunities_seen,
                          static_cast<uint64_t>(frame_opportunities.size()));

        for (const ArbOpportunity& opp : frame_opportunities) {
            push_opportunity(opp);
        }

        if (config.replay_logging_enabled) {
            auto was_touched = [&](const Contract* contract) {
                return contract->touched_frame_id == frame_id;
            };

            auto emit_replay = [&](std::string_view event_key, std::string_view event_label,
                                   const Contract& contract, uint8_t leg_index, uint8_t leg_count) {
                ReplaySnapshot snapshot{};
                snapshot.frame_id = frame_id;
                snapshot.t0_ws_recv_ns = slot->recv_time;
                snapshot.t1_parse_start_ns = t1_parse_start;
                snapshot.t2_parse_done_ns = t2_parse_done;
                snapshot.t3_books_done_ns = t3_books_done;
                snapshot.frame_bytes = static_cast<uint32_t>(slot->len);
                snapshot.book_events = frame.book_events;
                snapshot.price_change_events = frame.price_change_events;
                snapshot.bbo_events = frame.bbo_events;
                snapshot.trade_events = frame.trade_events;
                snapshot.event_key = event_key;
                snapshot.event_label = event_label;
                snapshot.contract_name = contract.asset_name;
                snapshot.leg_index = leg_index;
                snapshot.leg_count = leg_count;
                snapshot.touched_in_frame = was_touched(&contract);
                snapshot.yes_exchange_ts = contract.book_yes.timestamp;
                snapshot.no_exchange_ts = contract.book_no.timestamp;
                snapshot.yes_bid = contract.book_yes.best_bid;
                snapshot.yes_bid_size = contract.book_yes.best_bid_size;
                snapshot.yes_ask = contract.book_yes.best_ask;
                snapshot.yes_ask_size = contract.book_yes.best_ask_size;
                snapshot.no_bid = contract.book_no.best_bid;
                snapshot.no_bid_size = contract.book_no.best_bid_size;
                snapshot.no_ask = contract.book_no.best_ask;
                snapshot.no_ask_size = contract.book_no.best_ask_size;
                push_replay_snapshot(snapshot);
            };

            for (const ContractGroup* group : touched_groups) {
                for (size_t i = 0; i < group->contracts.size(); ++i) {
                    emit_replay(group->key, group->display_name, *group->contracts[i],
                                static_cast<uint8_t>(i), static_cast<uint8_t>(group->contracts.size()));
                }
            }

            for (Contract* contract : touched_contracts) {
                const auto group_it = contract_group_routes.find(contract);
                if (group_it != contract_group_routes.end() && !group_it->second.empty()) {
                    continue;
                }
                emit_replay(contract->condition_id, contract->asset_name, *contract, 0, 1);
            }
        }

        const NanoTime t4_arb_done = config.metrics_enabled ? now_ns() : 0;
        const float parse_us = config.metrics_enabled
            ? static_cast<float>(std::max<int64_t>(
                  0, static_cast<int64_t>(t2_parse_done - t1_parse_start - frame.book_apply_ns)) / 1000.0)
            : 0.0f;
        const float book_us = config.metrics_enabled
            ? static_cast<float>(frame.book_apply_ns / 1000.0)
            : 0.0f;
        const float arb_us = (config.metrics_enabled && arb_checks > 0)
            ? static_cast<float>((t4_arb_done - t3_books_done) / 1000.0)
            : 0.0f;
        const float e2e_us = (config.metrics_enabled && arb_checks > 0)
            ? static_cast<float>((t4_arb_done - slot->recv_time) / 1000.0)
            : 0.0f;

        auto push_frame_metric = [&](MetricsEventType type, uint16_t count, bool attach_latency_sample) {
            if (count == 0) return;
            MetricsEvent event{};
            event.type = type;
            event.event_count = count;
            event.has_latency_sample = attach_latency_sample;
            event.queue_us = queue_us;
            event.parse_us = parse_us;
            event.book_us = book_us;
            event.arb_us = arb_us;
            event.e2e_us = e2e_us;
            event.arb_checks = attach_latency_sample ? arb_checks : 0;
            event.arb_checked = attach_latency_sample && arb_checks > 0;
            push_metrics(event);
        };

        bool latency_attached = false;
        auto take_latency_slot = [&](uint16_t count) {
            if (count == 0 || latency_attached) return false;
            latency_attached = true;
            return true;
        };

        push_frame_metric(MetricsEventType::BOOK, frame.book_events, take_latency_slot(frame.book_events));
        push_frame_metric(MetricsEventType::PRICE_CHANGE, frame.price_change_events,
                          take_latency_slot(frame.price_change_events));
        push_frame_metric(MetricsEventType::BEST_BID_ASK, frame.bbo_events,
                          take_latency_slot(frame.bbo_events));
        if (frame.trade_events > 0) {
            MetricsEvent trade_event{};
            trade_event.type = MetricsEventType::TRADE;
            trade_event.event_count = frame.trade_events;
            push_metrics(trade_event);
        }

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
    const uint64_t replay_drops = dropped_replay_snapshots.load(std::memory_order_relaxed);
    const uint64_t backpressure_events = message_queue_backpressure_events.load(std::memory_order_relaxed);
    const uint64_t backpressure_spins = message_queue_backpressure_spins.load(std::memory_order_relaxed);
    const uint64_t max_message_bytes = max_message_bytes_seen.load(std::memory_order_relaxed);
    const uint64_t max_frame_opportunities = max_frame_opportunities_seen.load(std::memory_order_relaxed);

    std::printf("[PIPELINE] Queue peaks msg=%zu/%zu metrics=%zu/%zu opp=%zu/%zu replay=%zu/%zu\n",
                message_queue->peak_size(), message_queue->capacity(),
                metrics_queue->peak_size(), metrics_queue->capacity(),
                opportunity_queue->peak_size(), opportunity_queue->capacity(),
                replay_queue->peak_size(), replay_queue->capacity());
    std::printf("[PIPELINE] Max frame=%" PRIu64 " bytes | max frame opps=%" PRIu64
                " | msg backpressure=%" PRIu64 " events (%" PRIu64 " spins)\n",
                max_message_bytes, max_frame_opportunities, backpressure_events, backpressure_spins);
    std::printf("[PIPELINE] Dropped oversized=%" PRIu64
                " | dropped metrics=%" PRIu64
                " | dropped opportunities=%" PRIu64
                " | dropped replay=%" PRIu64 "\n",
                oversize_drops, metrics_drops, opportunity_drops, replay_drops);
    std::printf("[WEBSOCKET] attempts=%" PRIu64 " success=%" PRIu64
                " reconnects=%" PRIu64 " stale=%" PRIu64
                " timeouts=%" PRIu64 " read_errors=%" PRIu64
                " closes=%" PRIu64 " pings=%" PRIu64 "\n",
                websocket_stats.connect_attempts,
                websocket_stats.successful_connects,
                websocket_stats.reconnect_cycles,
                websocket_stats.stale_reconnects,
                websocket_stats.timeout_polls,
                websocket_stats.read_errors,
                websocket_stats.closed_events,
                websocket_stats.ping_count);

    std::vector<const Contract*> market_activity;
    market_activity.reserve(config.contracts.size());
    std::vector<const Contract*> zero_update_markets;
    zero_update_markets.reserve(config.contracts.size());
    for (const auto& contract : config.contracts) {
        market_activity.push_back(&contract);
        if (contract.total_updates == 0) {
            zero_update_markets.push_back(&contract);
        }
    }
    std::sort(market_activity.begin(), market_activity.end(),
              [](const Contract* lhs, const Contract* rhs) {
                  if (lhs->total_updates != rhs->total_updates) {
                      return lhs->total_updates > rhs->total_updates;
                  }
                  return lhs->asset_name < rhs->asset_name;
              });

    std::printf("[FEED] Markets with updates=%zu/%zu | zero-update=%zu\n",
                config.contracts.size() - zero_update_markets.size(),
                config.contracts.size(),
                zero_update_markets.size());
    const size_t report_limit = std::min(config.active_market_report_limit, market_activity.size());
    for (size_t i = 0; i < report_limit; ++i) {
        const Contract* contract = market_activity[i];
        if (contract->total_updates == 0) break;
        std::printf("[FEED] Top %-2zu %-40s | total=%" PRIu64 " yes=%" PRIu64 " no=%" PRIu64 "\n",
                    i + 1,
                    contract->asset_name.c_str(),
                    contract->total_updates,
                    contract->yes_updates,
                    contract->no_updates);
    }
    if (!zero_update_markets.empty()) {
        std::printf("[FEED] Zero-update markets:");
        const size_t zero_report_limit = std::min(config.active_market_report_limit, zero_update_markets.size());
        for (size_t i = 0; i < zero_report_limit; ++i) {
            std::printf(" %s%s",
                        zero_update_markets[i]->asset_name.c_str(),
                        i + 1 == zero_report_limit ? "" : ",");
        }
        if (zero_update_markets.size() > zero_report_limit) {
            std::printf(" ...");
        }
        std::printf("\n");
    }

    return 0;
}
