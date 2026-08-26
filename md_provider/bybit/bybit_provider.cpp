#include "bybit_provider.h"
#include "bybit_parser.h"
#include "types/venue.h"
#include <fmt/format.h>

using namespace market_data;

BybitProvider::BybitProvider(const ProviderConfig& config, CallBack callback) : Provider(config, std::move(callback)) {}

std::string BybitProvider::DepthSubscriptionMessage() const {
    return fmt::format(R"({{"op":"subscribe","args":["orderbook.50.{}"]}})",
                       VenueConverter::ToInstrumentString(config.instrument));
}

std::string BybitProvider::BboSubscriptionMessage() const {
    return fmt::format(R"({{"op":"subscribe","args":["orderbook.1.{}"]}})",
                       VenueConverter::ToInstrumentString(config.instrument));
}

void BybitProvider::OnDepthMessage(const std::string& message) {
    auto update = ParseBybitOrderbookMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not an orderbook message (e.g. subscribe ack, pong)
    }
    update->recv_ts_ns = GetCurrentTimeMs() * 1'000'000;
    // TODO: gap detection/resync (DESIGN_1 §4.2) not implemented yet -
    // "type":"snapshot" resets the book but continuity of "delta" messages
    // via `u`/`seq` is not checked.
    Emit(*update);
}

void BybitProvider::OnBboMessage(const std::string& message) {
    // TODO: fast-BBO correctness oracle (DESIGN_1 §4.4) not implemented yet -
    // this parses but nothing compares it against the depth-derived BBO or
    // triggers a resync. Never Emit()'d - see Provider::OnBboMessage docs.
    ParseBybitOrderbookMessage(message, config.venue_id, config.instrument);
}
