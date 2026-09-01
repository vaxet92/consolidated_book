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
    : Provider(config, std::move(callback), std::move(quote_callback)), parser_(config.depth) {}

std::string OKXProvider::DepthSubscriptionMessage() const {
    return fmt::format(R"({{"op":"subscribe","args":[{{"channel":"books","instId":"{}"}}]}})",
                       ToOkxInstId(config.instrument));
}

std::string OKXProvider::BboSubscriptionMessage() const {
    return fmt::format(R"({{"op":"subscribe","args":[{{"channel":"bbo-tbt","instId":"{}"}}]}})",
                       ToOkxInstId(config.instrument));
}

void OKXProvider::OnDepthMessage(const std::string& message, uint32_t conn_index) {
    auto update = parser_.ParseBooksMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not a books update (e.g. subscribe ack, pong)
    }

    // KEY: only the documented MAINTENANCE RESET counts - seqId jumps
    // backwards while prevSeqId still chains (prevSeqId=15, seqId=3). It is
    // not a gap (CheckOkxContinuity below treats it as kApply), but it does
    // put the id under our high-water mark, and without this flag the filter
    // would drop it and every message after it, forever, with nothing logged.
    // That is the silent freeze SeqDedup::LooksStuck() guards against.
    //
    // An ordinary snapshot (prevSeqId == -1) is deliberately NOT included.
    // Its seqId moves forward, so the `<=` rule already decides correctly, and
    // flagging it as a reset let a late-connecting socket's stale snapshot
    // drag the mark backwards - the live Bybit gap we hit.
    //
    // prev_seq > 0, not >= 0: -1 is the snapshot marker and 0 is what the
    // parser writes when the field is missing. Treating a parse failure as a
    // reset would be worse than treating it as a normal message.
    const bool venue_reset = (update->prev_seq > 0 && update->seq < static_cast<uint64_t>(update->prev_seq));

    if (!AcceptDepth(update->seq, conn_index, venue_reset)) {
        return;
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

    update->recv_ts_ns = GetCurrentTimeMs() * kTsNsMultiplier;

    Emit(*update);
}

void OKXProvider::OnBboMessage(const std::string& message, uint32_t conn_index) {
    auto update = parser_.ParseBboMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not a bbo-tbt payload (e.g. subscribe ack, pong)
    }

    // Separate filter from the depth stream: bbo-tbt and books carry
    // independent seqId sequences, so a shared high-water mark would silently
    // drop one stream behind the other.
    if (!AcceptBbo(update->seq, conn_index)) {
        return;
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
    quote.recv_ts_ns = GetCurrentTimeMs() * kTsNsMultiplier;
    quote.bid_price = update->bids.front().price;
    quote.bid_qty = update->bids.front().qty;
    quote.ask_price = update->asks.front().price;
    quote.ask_qty = update->asks.front().qty;
    EmitQuote(quote);
}
