#include "bybit_provider.h"
#include "bybit_parser.h"
#include "continuity.h"
#include "logger/logger.h"
#include "types/venue.h"
#include <fmt/format.h>

using namespace market_data;

BybitProvider::BybitProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback)
    : Provider(config, std::move(callback), std::move(quote_callback)), parser_(config.depth) {}

std::string BybitProvider::DepthSubscriptionMessage() const {
    // Depth is part of the TOPIC NAME on Bybit, not a parameter - already
    // resolved to a published tier (1/50/200/1000) by SelectDepthTier.
    return fmt::format(R"({{"op":"subscribe","args":["orderbook.{}.{}"]}})", config.depth,
                       VenueConverter::ToInstrumentString(config.instrument));
}

std::string BybitProvider::BboSubscriptionMessage() const {
    return fmt::format(R"({{"op":"subscribe","args":["orderbook.1.{}"]}})",
                       VenueConverter::ToInstrumentString(config.instrument));
}

void BybitProvider::OnDepthMessage(const std::string& message, uint32_t conn_index) {
    auto update = parser_.ParseOrderbookMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not an orderbook message (e.g. subscribe ack, pong)
    }

    // Redundant-connection dedup. Placement is the whole thing: AFTER the
    // parse, because the reset flag needs the parsed message, and BEFORE the
    // continuity check, because a duplicate looks to CheckBybitContinuity
    // like u != last_u + 1 - which it reports as a gap and resyncs on. Three
    // connections would then cause two spurious resyncs per message.
    //
    // KEY: only the u == 1 SERVICE RESTART counts as a venue reset - that is
    // the one case where the id genuinely moves backwards. An ordinary
    // snapshot must pass false: its id moves forward, so the `<=` rule
    // decides correctly on its own (newer than us, apply; older, drop).
    //
    // Passing update->is_snapshot here instead caused a live gap. All three
    // sockets are created before any connects, so a socket that connects LAST
    // opened with a snapshot behind the others, was honoured as a reset, and
    // dragged the high-water mark backwards - after which the next healthy
    // message read as a forward gap and resynced.
    const bool venue_reset = (update->seq == 1);

    if (!AcceptDepth(update->seq, conn_index, venue_reset)) {
        return;
    }

    switch (CheckBybitContinuity(*update, last_depth_u_)) {
        case ContinuityAction::kIgnore:
            return;
        case ContinuityAction::kGap:
            // The book is now WRONG, not merely stale (§4.2). Applying this
            // delta would silently corrupt it, so drop the book and
            // re-subscribe to get a fresh snapshot.
            Logger::Log(LogLevel::kWarning, "[BYBIT] depth gap: expected u={}, got {} - resyncing", last_depth_u_ + 1,
                        update->seq);
            last_depth_u_ = 0;
            RequestResync();
            return;
        case ContinuityAction::kReset:
        case ContinuityAction::kApply:
            break;
    }

    update->recv_ts_ns = GetCurrentTimeMs() * kTsNsMultiplier;
    Emit(*update);
}

void BybitProvider::OnBboMessage(const std::string& message, uint32_t conn_index) {
    auto update = parser_.ParseOrderbookMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not an orderbook message (e.g. subscribe ack, pong)
    }

    // Separate filter from the depth stream: orderbook.1 and orderbook.50
    // carry independent `u` sequences, so a shared high-water mark would
    // silently drop one stream behind the other.
    if (!AcceptBbo(update->seq, conn_index)) {
        return;
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
    quote.recv_ts_ns = GetCurrentTimeMs() * kTsNsMultiplier;
    quote.bid_price = update->bids.front().price;
    quote.bid_qty = update->bids.front().qty;
    quote.ask_price = update->asks.front().price;
    quote.ask_qty = update->asks.front().qty;
    EmitQuote(quote);
}
