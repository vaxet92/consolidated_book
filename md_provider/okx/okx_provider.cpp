#include "okx_provider.h"
#include "okx_parser.h"
#include "continuity.h"
#include "logger/logger.h"
#include "types/venue.h"
#include <fmt/format.h>

using namespace market_data;

namespace {

// OKX uses "BTC-USDT", not "BTCUSDT" - insert a hyphen before the quote
// currency. Simple and matches every symbol this project uses (all end in
// USDT).
std::string ToOkxInstId(InstrumentId instrument) {
    std::string symbol = VenueConverter::ToInstrumentString(instrument);
    size_t usdt_pos = symbol.find("USDT");
    if (usdt_pos != std::string::npos) {
        symbol.insert(usdt_pos, "-");
    }
    return symbol;
}

}  // namespace

OKXProvider::OKXProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback)
    : Provider(config, std::move(callback), std::move(quote_callback)) {}

std::string OKXProvider::DepthSubscriptionMessage() const {
    return fmt::format(R"({{"op":"subscribe","args":[{{"channel":"books","instId":"{}"}}]}})",
                       ToOkxInstId(config.instrument));
}

std::string OKXProvider::BboSubscriptionMessage() const {
    return fmt::format(R"({{"op":"subscribe","args":[{{"channel":"bbo-tbt","instId":"{}"}}]}})",
                       ToOkxInstId(config.instrument));
}

void OKXProvider::OnDepthMessage(const std::string& message) {
    auto update = ParseOkxBooksMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not a books update (e.g. subscribe ack, pong)
    }

    switch (CheckOkxContinuity(*update, last_depth_seq_)) {
        case ContinuityAction::kIgnore:
            return;
        case ContinuityAction::kGap:
            // Chain broken: the book is WRONG, not merely stale (§4.2).
            Logger::Log(LogLevel::kWarning, "[OKX] depth gap: expected prevSeqId={}, got {} - resyncing",
                        last_depth_seq_, update->prev_seq);
            last_depth_seq_ = 0;
            RequestResync();
            return;
        case ContinuityAction::kReset:
        case ContinuityAction::kApply:
            break;
    }

    update->recv_ts_ns = GetCurrentTimeMs() * 1'000'000;

    Emit(*update);
}

void OKXProvider::OnBboMessage(const std::string& message) {
    auto update = ParseOkxBboMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not a bbo-tbt payload (e.g. subscribe ack, pong)
    }

    // bbo-tbt always carries both sides, one level each (verified against
    // live capture) - a full replacement every message, no deltas.
    if (update->bids.empty() || update->asks.empty()) {
        return;
    }

    BboQuote quote;
    quote.venue = config.venue_id;
    quote.instrument = config.instrument;
    quote.seq = update->seq;  // OKX seqId
    quote.exch_ts_ns = update->exch_ts_ns;
    quote.recv_ts_ns = GetCurrentTimeMs() * 1'000'000;
    quote.bid_price = update->bids.front().price;
    quote.bid_qty = update->bids.front().qty;
    quote.ask_price = update->asks.front().price;
    quote.ask_qty = update->asks.front().qty;
    EmitQuote(quote);
}
