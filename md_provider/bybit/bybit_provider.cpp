#include "bybit_provider.h"
#include "bybit_parser.h"
#include "types/venue.h"
#include <fmt/format.h>

using namespace market_data;

BybitProvider::BybitProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback)
    : Provider(config, std::move(callback), std::move(quote_callback)) {}

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
    auto update = ParseBybitOrderbookMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not an orderbook message (e.g. subscribe ack, pong)
    }

    // orderbook.1 sends every message as "type":"snapshot" with both sides
    // present (verified against live capture) - so no carried-over state is
    // needed, unlike a real delta stream. Guard anyway rather than publish
    // a half-formed quote if that ever changes.
    if (update->bids.empty() || update->asks.empty()) {
        return;
    }

    BboQuote quote;
    quote.venue = config.venue_id;
    quote.instrument = config.instrument;
    quote.seq = update->seq;  // Bybit `u` - increments by 1 per message on orderbook.1
    quote.exch_ts_ns = update->exch_ts_ns;
    quote.recv_ts_ns = GetCurrentTimeMs() * 1'000'000;
    quote.bid_price = update->bids.front().price;
    quote.bid_qty = update->bids.front().qty;
    quote.ask_price = update->asks.front().price;
    quote.ask_qty = update->asks.front().qty;
    EmitQuote(quote);
}
