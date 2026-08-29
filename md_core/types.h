#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "types/venue.h"

namespace market_data {

using PriceTicks = uint64_t;         // price x 1e8, integral on the canonical grid. Never negative.
using QtyUnits = uint64_t;           // base quantity x 1e8. Never negative.
using Notional = unsigned __int128;  // ticks * units - needs the extra width. Never negative.

// The fixed scale behind PriceTicks/QtyUnits: the real value x 1e8. Both
// forms are needed - the exponent goes on the wire (Update.price_scale), the
// factor is used in band arithmetic to convert between the raw price x qty
// product (x 1e16) and a notional in quote currency (x 1e8).
inline constexpr uint32_t kScaleExponent = 8;
inline constexpr uint64_t kScaleFactor = 100'000'000;

// Any place that needs a *signed* difference between two PriceTicks/QtyUnits
// (e.g. a crossed book: bid - ask) must compute it explicitly as
// (bool sign, PriceTicks magnitude) - compare first, then subtract the
// smaller from the larger. Never subtract two of these directly; the result
// can silently wrap instead of going negative.

struct PriceLevel {
    PriceTicks price;
    QtyUnits qty;  // absolute qty AT this price; qty == 0 means "remove this level"
};

struct BookUpdate {
    VenueId venue;
    InstrumentId instrument;
    uint64_t seq;  // venue-native monotonic sequence number
    // Venue-specific continuity field: the sequence this update claims to
    // follow. OKX `prevSeqId` (-1 on a snapshot), Binance `U` (first update
    // id in the event). 0 when the venue has no such field - Bybit chains
    // by u+1 instead. Signed because OKX uses -1.
    int64_t prev_seq = 0;
    int64_t recv_ts_ns;  // CLOCK_MONOTONIC, ours. Signed: used in drift subtraction.
    int64_t exch_ts_ns;  // venue's own timestamp - drift estimation only, never compared across venues.
    bool is_snapshot;    // true = full replace, false = incremental delta
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};

// Top-of-book from a venue's fast-BBO stream (Binance @bookTicker, Bybit
// orderbook.1, OKX bbo-tbt). Deliberately NOT a BookUpdate and never
// applied to a VenueBook: the two streams are not mutually sequenced, and
// splicing fast-BBO into the depth book corrupts it (DESIGN_1 §4.4).
struct BboQuote {
    VenueId venue;
    InstrumentId instrument;
    uint64_t seq = 0;        // venue-native: Binance `u`, Bybit `seq`, OKX `seqId`
    int64_t recv_ts_ns = 0;  // ours, stamped by the provider
    int64_t exch_ts_ns = 0;  // venue's, where available (Binance bookTicker has none)
    PriceTicks bid_price = 0;
    QtyUnits bid_qty = 0;
    PriceTicks ask_price = 0;
    QtyUnits ask_qty = 0;
};

// One latest fast-BBO quote per venue, indexed by VenueId. A quote with
// price == 0 means that venue hasn't sent one yet. Raw per-venue input to
// consolidation, not consolidated output - which is why it lives here at
// market_data scope, mirroring VenueBookArray, rather than in the
// `consolidated` namespace.
using VenueQuoteArray = std::array<BboQuote, kVenueCount>;

// Core's own config, typed (VenueId/InstrumentId), not raw strings.
// Whoever loads the config file (main.cpp) translates strings to enums
// once, at the boundary - Core never parses a string.
struct CoreConfig {
    std::vector<VenueId> venues;                    // which venues are enabled
    std::vector<InstrumentId> default_instruments;  // subscribed at startup
};

}  // namespace market_data
