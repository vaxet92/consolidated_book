#pragma once

#include <optional>
#include <string>

#include "md_core/types.h"

namespace market_data {

// Parses one OKX "books"/"bbo-tbt" channel message (public, spot). Uses the
// first entry of the "data" array - OKX sends exactly one per instId
// subscribed. Returns std::nullopt for non-books messages (subscribe acks,
// pongs, an empty data array). Never throws.
//
// KEY: fields inside the data[] entry are read in the SAME order OKX sends
// them: asks, bids, ts, checksum (skipped), seqId. An earlier version of
// this code read ts/seqId before bids/asks - simdjson on-demand is
// forward-only, so that order crashes (assertion, not a normal error), the
// same bug that hit BybitProvider live. test_okx_parser.cpp pins this down.
std::optional<BookUpdate> ParseOkxBooksMessage(const std::string& message, VenueId venue, InstrumentId instrument);

}  // namespace market_data
