#pragma once

#include <optional>
#include <string>

#include "md_core/types.h"

namespace market_data {

// Parses one Binance depthUpdate message (spot, e.g. btcusdt@depth@100ms).
// Returns std::nullopt for non-depth messages (subscribe acks). Never
// throws - malformed JSON also becomes std::nullopt.
std::optional<BookUpdate> ParseBinanceDepthMessage(const std::string& message, VenueId venue,
                                                    InstrumentId instrument);

// Parses one Binance bookTicker message: {"u","s","b","B","a","A"} - always
// carries both sides, no deltas, so a complete BboQuote comes from every
// message with no carried-over state. `u` is the order-book update ID.
// Returns std::nullopt for non-bookTicker messages. Never throws.
std::optional<BboQuote> ParseBinanceBboMessage(const std::string& message, VenueId venue, InstrumentId instrument);

}  // namespace market_data
