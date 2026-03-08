#include "websocket.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

WebSocketClient::WebSocketClient(const Config& config)
    : config_(config)
    , ssl_ctx_(ssl::context::tlsv12_client)
{
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
    buffer_.reserve(256 * 1024);
}

WebSocketClient::~WebSocketClient() {
    stop();
}

bool WebSocketClient::connect() {
    int delay = 1;
    while (running_) {
        if (do_connect() && do_subscribe()) {
            connected_ = true;
            return true;
        }
        std::cerr << "[RECONNECT] Waiting " << delay << "s before retry...\n";
        std::this_thread::sleep_for(std::chrono::seconds(delay));
        delay = std::min(delay * 2, config_.reconnect_max_delay_seconds);
    }
    // First attempt (not in reconnect loop)
    if (do_connect() && do_subscribe()) {
        connected_ = true;
        return true;
    }
    return false;
}

bool WebSocketClient::do_connect() {
    try {
        NanoTime t_start = now_ns();

        // Reset the WebSocket stream (use beast::tcp_stream for timeout support)
        ws_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            ioc_, ssl_ctx_);

        // DNS resolve
        tcp::resolver resolver(ioc_);
        NanoTime t_dns_start = now_ns();
        auto const results = resolver.resolve(config_.websocket_host,
                                              std::to_string(config_.websocket_port));
        NanoTime t_dns_done = now_ns();
        double dns_ms = (t_dns_done - t_dns_start) / 1e6;
        printf("[PERF] DNS resolve: %.2fms\n", dns_ms);

        // TCP connect (beast::tcp_stream uses its own connect method)
        NanoTime t_tcp_start = now_ns();
        beast::get_lowest_layer(*ws_).connect(results);
        beast::get_lowest_layer(*ws_).socket().set_option(tcp::no_delay(true));
        NanoTime t_tcp_done = now_ns();
        double tcp_ms = (t_tcp_done - t_tcp_start) / 1e6;
        printf("[PERF] TCP connect: %.2fms\n", tcp_ms);

        // SNI hostname
        if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(),
                                       config_.websocket_host.c_str())) {
            throw beast::system_error(
                beast::error_code(static_cast<int>(::ERR_get_error()),
                                  net::error::get_ssl_category()),
                "Failed to set SNI hostname");
        }

        // TLS handshake
        NanoTime t_tls_start = now_ns();
        ws_->next_layer().handshake(ssl::stream_base::client);
        NanoTime t_tls_done = now_ns();
        double tls_ms = (t_tls_done - t_tls_start) / 1e6;
        printf("[PERF] TLS handshake: %.2fms\n", tls_ms);

        // WebSocket upgrade
        ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(boost::beast::http::field::user_agent, "polymarket-arb/1.0");
            }));

        NanoTime t_ws_start = now_ns();
        ws_->handshake(config_.websocket_host + ":" + std::to_string(config_.websocket_port),
                       config_.websocket_path);
        NanoTime t_ws_done = now_ns();
        double ws_ms = (t_ws_done - t_ws_start) / 1e6;
        printf("[PERF] WS handshake: %.2fms\n", ws_ms);

        double total_ms = (t_ws_done - t_start) / 1e6;
        printf("[PERF] Total connect: %.2fms (DNS=%.1f + TCP=%.1f + TLS=%.1f + WS=%.1f)\n",
               total_ms, dns_ms, tcp_ms, tls_ms, ws_ms);
        printf("[CONNECTED] WebSocket upgrade complete\n");

        // Auto-respond to pings
        ws_->control_callback(
            [](websocket::frame_type kind, beast::string_view payload) {
                if (kind == websocket::frame_type::ping) {
                    // boost::beast auto-pongs
                }
            });

        return true;

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Connection failed: " << e.what() << std::endl;
        connected_ = false;
        return false;
    }
}

bool WebSocketClient::do_subscribe() {
    try {
        // Build subscription message with all token IDs
        std::ostringstream sub;
        sub << R"({"assets_ids":[)";
        bool first = true;
        for (const auto& c : config_.contracts) {
            if (!c.token_id_yes.empty()) {
                if (!first) sub << ",";
                sub << "\"" << c.token_id_yes << "\"";
                first = false;
            }
            if (!c.token_id_no.empty()) {
                if (!first) sub << ",";
                sub << "\"" << c.token_id_no << "\"";
                first = false;
            }
        }
        sub << R"(],"type":"market","initial_dump":)"
            << (config_.initial_dump ? "true" : "false")
            << R"(,"custom_feature_enabled":)"
            << (config_.custom_feature_enabled ? "true" : "false")
            << "}";

        std::string msg = sub.str();
        ws_->write(net::buffer(msg));

        // Print abbreviated token IDs
        std::cout << "[SUBSCRIBED] " << config_.contracts.size() << " contract(s): ";
        for (const auto& c : config_.contracts) {
            std::cout << c.asset_name;
            if (&c != &config_.contracts.back()) std::cout << ", ";
        }
        std::cout << std::endl;

        return true;

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Subscribe failed: " << e.what() << std::endl;
        return false;
    }
}

void WebSocketClient::run(MessageCallback on_message) {
    running_ = true;
    auto last_ping = std::chrono::steady_clock::now();
    auto last_msg = std::chrono::steady_clock::now();

    while (running_) {
        if (!connected_) {
            std::cerr << "[RECONNECT] Attempting reconnection...\n";
            if (!connect()) {
                if (running_) {
                    std::cerr << "[RECONNECT] Stopping after failed reconnect\n";
                }
                break;
            }
            last_ping = std::chrono::steady_clock::now();
            last_msg = last_ping;
        }

        try {
            beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(5));

            buffer_.clear();
            beast::error_code ec;
            ws_->read(buffer_, ec);

            NanoTime recv_time = now_ns();

            if (ec) {
                if (ec == beast::error::timeout) {
                    auto now = std::chrono::steady_clock::now();
                    auto since_msg =
                        std::chrono::duration_cast<std::chrono::seconds>(now - last_msg).count();

                    if (since_msg > 30) {
                        std::cerr << "[STALE] No message for " << since_msg << "s, reconnecting...\n";
                        connected_ = false;
                        continue;
                    }

                    auto since_ping =
                        std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count();
                    if (since_ping >= config_.ping_interval_seconds) {
                        send_ping();
                        last_ping = now;
                    }
                    continue;
                }

                if (ec == websocket::error::closed) {
                    std::cout << "[CLOSED] WebSocket closed by server\n";
                } else {
                    std::cerr << "[ERROR] Read error: " << ec.message() << std::endl;
                }
                connected_ = false;
                continue;
            }

            last_msg = std::chrono::steady_clock::now();

            auto buf_data = buffer_.data();
            const char* raw_ptr = static_cast<const char*>(buf_data.data());
            size_t raw_len = buf_data.size();
            on_message(raw_ptr, raw_len, recv_time);

        } catch (const std::exception& e) {
            std::cerr << "[ERROR] " << e.what() << std::endl;
            connected_ = false;
        }
    }
}

void WebSocketClient::send_ping() {
    try {
        ws_->ping({});
    } catch (const std::exception& e) {
        std::cerr << "[PING ERROR] " << e.what() << std::endl;
    }
}

void WebSocketClient::stop() {
    running_ = false;
    connected_ = false;
    try {
        if (ws_ && ws_->is_open()) {
            ws_->close(websocket::close_code::normal);
        }
    } catch (...) {}
}
