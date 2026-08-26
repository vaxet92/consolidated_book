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

// Top-of-book only, from Binance's bookTicker stream. Distinct shape from
// depthUpdate (flat top-level b/B/a/A fields, no nested book) - not a
// BookUpdate, since there is no "rest of the book" in this message.
struct BboQuote {
    PriceTicks bid_price;
    QtyUnits bid_qty;
    PriceTicks ask_price;
    QtyUnits ask_qty;
};

// Returns std::nullopt for non-bookTicker messages. Never throws.
std::optional<BboQuote> ParseBinanceBboMessage(const std::string& message);

}  // namespace market_data
