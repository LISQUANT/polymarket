#pragma once

#include "types.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#include <string>
#include <functional>
#include <atomic>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class WebSocketClient {
public:
    // Zero-copy callback: raw pointer + length from beast buffer, no std::string
    using MessageCallback = std::function<void(const char* data, size_t len, NanoTime recv_time)>;

    explicit WebSocketClient(const Config& config);
    ~WebSocketClient();

    // Connect and subscribe to all tokens in config
    bool connect();

    // Start reading messages (blocks until disconnect or error)
    void run(MessageCallback on_message);

    // Graceful shutdown
    void stop();

    bool is_connected() const { return connected_; }

private:
    const Config& config_;
    net::io_context ioc_;
    ssl::context ssl_ctx_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer buffer_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};

    bool do_connect();
    bool do_subscribe();
    void send_ping();
};
