#pragma once

#include <optional>
#include <string>

#include "md_core/types.h"

namespace market_data {

// Parses one OKX "books" channel message (depth, public spot):
// {"arg","action","data":[{"asks","bids","ts","checksum","seqId"}]}
// Uses the first entry of the "data" array - OKX sends exactly one per
// instId subscribed. Returns std::nullopt for non-books messages (subscribe
// acks, pongs, an empty data array) and for malformed JSON. Never throws.
//
// KEY: fields inside the data[] entry are read in the SAME order OKX sends
// them: asks, bids, ts, checksum (skipped), seqId. An earlier version of
// this code read ts/seqId before bids/asks - simdjson on-demand is
// forward-only, so that order crashes (assertion, not a normal error), the
// same bug that hit BybitProvider live. test_okx_parser.cpp pins this down.
std::optional<BookUpdate> ParseOkxBooksMessage(const std::string& message, VenueId venue, InstrumentId instrument);

// Parses one OKX "bbo-tbt" channel message (fast top-of-book, public spot):
// {"arg","data":[{"asks","bids","ts","seqId"}]}
//
// Deliberately a separate function from ParseOkxBooksMessage rather than
// making `action` optional there: bbo-tbt has no `action` field and no
// `checksum`, so it is a genuinely different shape. Sharing one parser meant
// probing for an absent field, and on malformed input that probe leaves
// simdjson's lazy iterator at a broken depth - the next lookup then asserts
// instead of returning an error. Gating on `data` (always present here)
// keeps the read strictly forward-only. Never throws.
std::optional<BookUpdate> ParseOkxBboMessage(const std::string& message, VenueId venue, InstrumentId instrument);

}  // namespace market_data
