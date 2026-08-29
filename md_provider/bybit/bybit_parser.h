#pragma once

#include <optional>
#include <string>

#include "md_core/types.h"

namespace market_data {

// Parses one Bybit orderbook.* message (v5 public/spot: {"topic","type",
// "ts","data":{...},"cts"}). Returns std::nullopt for non-orderbook
// messages (subscribe acks, pongs).
//
// KEY: fields below are read in the SAME order they appear in the real
// message (topic, type, ts, then inside data: u, b, a). simdjson's
// on-demand API is a single forward pass - it can never read a field that
// comes before one already consumed. Reordering these reads without
// checking the real wire format will crash (assertion in simdjson, not a
// normal error), not just misparse. test_bybit_parser.cpp pins this down
// against a real sample payload.
std::optional<BookUpdate> ParseBybitOrderbookMessage(const std::string& message, VenueId venue,
                                                     InstrumentId instrument);

}  // namespace market_data
