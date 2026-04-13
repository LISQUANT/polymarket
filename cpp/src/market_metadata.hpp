#pragma once

#include <cstddef>
#include <string>
#include <string_view>

struct MarketMetadata {
    bool available = false;
    bool fees_enabled = false;
    double fee_rate = 0.0;
    int fee_exponent = 0;
    int base_fee_bps = -1;
    bool neg_risk = false;
    std::string event_slug;
    std::string event_title;
};

class MarketMetadataClient {
public:
    bool fetch_market_by_token(std::string_view token_id, MarketMetadata& out, std::string& error);
    bool fetch_event_market_count(std::string_view event_slug, size_t& market_count, std::string& error);

private:
    bool https_get(std::string_view host, std::string_view target, std::string& body, std::string& error);
};
