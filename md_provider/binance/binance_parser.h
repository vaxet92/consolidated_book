#pragma once

#include <optional>
#include <string>

#include "md_core/types.h"

namespace market_data {

// Parses one Binance depthUpdate message (spot, e.g. btcusdt@depth@100ms):
// {"e","E","s","U","u","b","a"}. `u` (final update id) goes to seq, `U`
// (first update id in the event) goes to prev_seq - the sync reconciliation
// and the continuity chain both need it. Returns std::nullopt for non-depth
// messages (subscribe acks). Never throws - malformed JSON also becomes
// std::nullopt.
std::optional<BookUpdate> ParseBinanceDepthMessage(const std::string& message, VenueId venue,
                                                    InstrumentId instrument);

// Parses the REST depth snapshot body from GET /api/v3/depth:
// {"lastUpdateId": N, "bids": [[px,qty],...], "asks": [[px,qty],...]}
//
// A genuinely different shape from the WS depthUpdate - no e/E/U/u - so it
// gets its own parser rather than a flag on the other one. Returns a
// BookUpdate with is_snapshot = true and seq = lastUpdateId. Never throws.
std::optional<BookUpdate> ParseBinanceDepthSnapshot(const std::string& body, VenueId venue, InstrumentId instrument);

// Parses one Binance bookTicker message: {"u","s","b","B","a","A"} - always
// carries both sides, no deltas, so a complete BboQuote comes from every
// message with no carried-over state. `u` is the order-book update ID.
// Returns std::nullopt for non-bookTicker messages. Never throws.
std::optional<BboQuote> ParseBinanceBboMessage(const std::string& message, VenueId venue, InstrumentId instrument);

}  // namespace market_data
