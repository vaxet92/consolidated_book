#pragma once

#include <optional>
#include <string_view>

#include "md_core/types.h"
#include "md_provider/base_parser.h"

namespace market_data {

// Binance sends prices/quantities as decimal strings; keep 8 fractional
// digits to match PriceTicks/QtyUnits.
inline constexpr uint64_t kBinanceScale = 8;

// Stateful parser for one Binance stream. Holds a reusable simdjson parser
// and input buffer, so a steady stream of messages does no per-message
// allocation.
//
// NOT thread-safe: one instance serves one thread. The depth and bookTicker
// handlers run on the same io_context thread and may share one instance; the
// REST snapshot runs on its own thread and needs its own instance.
class BinanceParser : public Parser {
   public:
    // venue_depth: this venue's resolved book-depth tier (ProviderConfig::depth).
    // The bids/asks vectors of each parsed update are reserved to this size, so
    // a full depth snapshot fills them without a reallocation. A @100ms delta
    // carries fewer levels and only uses part of it.
    explicit BinanceParser(uint32_t venue_depth);

    // One Binance spot depthUpdate (e.g. btcusdt@depth@100ms):
    // {"e","E","s","U","u","b","a"}. `u` -> seq, `U` -> prev_seq (both the
    // sync reconciliation and the continuity chain need `U`). Returns
    // std::nullopt for non-depth messages (subscribe acks) and for malformed
    // JSON - never throws.
    std::optional<BookUpdate> ParseDepthMessage(std::string_view message, VenueId venue, InstrumentId instrument);

    // The REST depth snapshot body from GET /api/v3/depth:
    // {"lastUpdateId": N, "bids": [[px,qty],...], "asks": [[px,qty],...]}.
    // A different shape from depthUpdate (no e/E/U/u), so its own method.
    // Returns is_snapshot = true, seq = lastUpdateId. Never throws.
    std::optional<BookUpdate> ParseDepthSnapshot(std::string_view body, VenueId venue, InstrumentId instrument);

    // One Binance bookTicker message: {"u","s","b","B","a","A"} - always both
    // sides, no deltas, so a complete BboQuote comes from every message with
    // no carried-over state. `u` is the order-book update ID. Returns
    // std::nullopt for non-bookTicker messages. Never throws.
    std::optional<BboQuote> ParseBboMessage(std::string_view message, VenueId venue, InstrumentId instrument);
};

}  // namespace market_data
