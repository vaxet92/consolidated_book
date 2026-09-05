#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "md_core/types.h"
#include "md_provider/base_parser.h"

namespace market_data {

// Stateful parser for one OKX connection. Reuses the base Parser's simdjson
// parser and input buffer, so a steady stream of messages does no per-message
// allocation. NOT thread-safe: one instance per thread. OKX gets its
// snapshot on the WS stream (action/type == "snapshot"), so unlike Binance
// there is no separate REST-snapshot path.
class OkxParser : public Parser {
   public:
    // venue_depth: this venue's resolved book-depth tier (ProviderConfig::depth).
    // The bids/asks vectors of each parsed update are reserved to this size.
    explicit OkxParser(uint32_t venue_depth);

    // One OKX "books" channel message (depth, public spot):
    // {"arg","action","data":[{"asks","bids","ts","checksum","prevSeqId","seqId"}]}
    // Uses the first entry of "data" - OKX sends exactly one per instId.
    // Returns std::nullopt for non-books messages (subscribe acks, pongs, an
    // empty data array) and for malformed JSON. Never throws.
    //
    // KEY: fields inside the data[] entry are read in the SAME order OKX
    // sends them: asks, bids, ts, checksum (skipped), prevSeqId, seqId.
    // simdjson on-demand is forward-only; reading ts/seqId before bids/asks
    // asserts (not a normal error). test_okx_parser.cpp pins this down.
    std::optional<BookUpdate> ParseBooksMessage(std::string_view message, VenueId venue, InstrumentKey instrument);

    // One OKX "bbo-tbt" channel message (fast top-of-book, public spot):
    // {"arg","data":[{"asks","bids","ts","seqId"}]}
    //
    // A separate method rather than a flag on ParseBooksMessage: bbo-tbt has
    // no `action` and no `checksum`, so it is a genuinely different shape.
    // Gating on `data` (always present here) keeps the read forward-only.
    // Never throws.
    std::optional<BookUpdate> ParseBboMessage(std::string_view message, VenueId venue, InstrumentKey instrument);
};

}  // namespace market_data
