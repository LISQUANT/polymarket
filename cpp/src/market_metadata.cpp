#include "market_metadata.hpp"

#include <simdjson.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <openssl/ssl.h>

#include <sstream>
#include <stdexcept>
#include <string>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace {

bool parse_market_metadata(std::string_view json, MarketMetadata& out, std::string& error) {
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(json);
    auto doc = parser.iterate(padded);

    simdjson::ondemand::array markets;
    if (doc.get_array().get(markets)) {
        error = "Gamma market payload was not an array";
        return false;
    }

    for (auto elem : markets) {
        simdjson::ondemand::object obj;
        if (elem.get_object().get(obj)) continue;

        bool bv = false;
        int64_t iv = 0;
        double dv = 0.0;
        std::string_view sv;

        out.available = true;
        if (!obj["feesEnabled"].get_bool().get(bv)) out.fees_enabled = bv;
        if (!obj["negRisk"].get_bool().get(bv)) out.neg_risk = bv;
        if (!obj["takerBaseFee"].get_int64().get(iv)) out.base_fee_bps = static_cast<int>(iv);

        simdjson::ondemand::object fee_schedule;
        if (!obj["feeSchedule"].get_object().get(fee_schedule)) {
            if (!fee_schedule["rate"].get_double().get(dv)) out.fee_rate = dv;
            if (!fee_schedule["exponent"].get_int64().get(iv)) out.fee_exponent = static_cast<int>(iv);
        }

        simdjson::ondemand::array events;
        if (!obj["events"].get_array().get(events)) {
            for (auto event_elem : events) {
                simdjson::ondemand::object event_obj;
                if (event_elem.get_object().get(event_obj)) continue;
                if (!event_obj["slug"].get_string().get(sv)) out.event_slug = std::string(sv);
                if (!event_obj["title"].get_string().get(sv)) out.event_title = std::string(sv);
                break;
            }
        }

        return true;
    }

    error = "No market metadata returned";
    return false;
}

bool parse_event_market_count(std::string_view json, size_t& market_count, std::string& error) {
    market_count = 0;

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(json);
    auto doc = parser.iterate(padded);

    simdjson::ondemand::array events;
    if (doc.get_array().get(events)) {
        error = "Gamma event payload was not an array";
        return false;
    }

    for (auto elem : events) {
        simdjson::ondemand::object obj;
        if (elem.get_object().get(obj)) continue;

        simdjson::ondemand::array markets;
        if (obj["markets"].get_array().get(markets)) {
            error = "Event did not include a markets array";
            return false;
        }

        size_t count = 0;
        for (auto ignored : markets) {
            (void)ignored;
            ++count;
        }
        market_count = count;
        return true;
    }

    error = "No event metadata returned";
    return false;
}

}  // namespace

bool MarketMetadataClient::https_get(std::string_view host, std::string_view target,
                                     std::string& body, std::string& error) {
    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tls_client};
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

        std::string host_str(host);
        std::string target_str(target);

        if (!SSL_set_tlsext_host_name(stream.native_handle(), host_str.c_str())) {
            error = "Failed to set TLS SNI host";
            return false;
        }

        const auto endpoints = resolver.resolve(host_str, "443");
        beast::get_lowest_layer(stream).connect(endpoints);
        stream.handshake(ssl::stream_base::client);

        http::request<http::empty_body> req{http::verb::get, target_str, 11};
        req.set(http::field::host, host_str);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::accept, "application/json");

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        if (res.result() != http::status::ok) {
            std::ostringstream oss;
            oss << "HTTP " << res.result_int() << " from " << host_str << target_str;
            error = oss.str();
            return false;
        }

        body = std::move(res.body());

        beast::error_code ec;
        stream.shutdown(ec);
        if (ec == net::error::eof || ec == ssl::error::stream_truncated) {
            ec = {};
        }
        if (ec) {
            error = ec.message();
            return false;
        }

        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool MarketMetadataClient::fetch_market_by_token(std::string_view token_id, MarketMetadata& out,
                                                 std::string& error) {
    std::string body;
    std::string target = "/markets?clob_token_ids=";
    target.append(token_id.data(), token_id.size());
    if (!https_get("gamma-api.polymarket.com", target, body, error)) {
        return false;
    }
    return parse_market_metadata(body, out, error);
}

bool MarketMetadataClient::fetch_event_market_count(std::string_view event_slug, size_t& market_count,
                                                    std::string& error) {
    std::string body;
    std::string target = "/events?slug=";
    target.append(event_slug.data(), event_slug.size());
    if (!https_get("gamma-api.polymarket.com", target, body, error)) {
        return false;
    }
    return parse_event_market_count(body, market_count, error);
}
